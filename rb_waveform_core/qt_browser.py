"""Touch browser in the style of a deck's own: a source tile and categories down the left, a back
bar and search across the top, drill-down lists and tracks in the middle, and a load bar below.
Queries run on one worker so the waveforms never wait for the library."""
from collections import OrderedDict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from PySide6.QtCore import Qt, QTimer, QAbstractTableModel, QAbstractListModel, QModelIndex, QRectF, QPointF, QSize, Signal
from PySide6.QtGui import QColor, QFont, QImage, QPainter, QPen, QPainterPath
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QPushButton, QAbstractButton, QLineEdit, QLabel, QTableView,
    QListView, QHeaderView, QAbstractItemView, QScroller, QStackedWidget, QStyledItemDelegate, QFrame, QGridLayout, QStyle)

from .catalog import SORTS, ORDER, sources, source_stats, categories, browse

BLUE = '#1a79eb'
PAGE = 200
NAV = (('PLAYLIST', 'PLAYLIST'), ('TRACK', 'TRACK'), ('ARTIST', 'ARTIST'), ('ALBUM', 'ALBUM'),
       ('KEY', 'KEY'), ('HISTORY', 'HISTORY'), ('SEARCH', 'SEARCH'))


def clock(seconds):
    seconds = int(max(0, seconds))
    return f'{seconds // 60:02}:{seconds % 60:02}'


def font(size, bold=False):
    f = QFont('Arial')
    f.setPixelSize(max(9, round(size)))
    f.setBold(bold)
    return f


class Artwork:
    """Small cache of decoded sleeves, shared by rows and the load bar."""
    def __init__(self, capacity=400):
        self.images = OrderedDict()
        self.capacity = capacity

    def get(self, path):
        if not path:
            return None
        if path in self.images:
            self.images.move_to_end(path)
            return self.images[path]
        image = QImage(path)
        image = None if image.isNull() else image.scaled(96, 96, Qt.AspectRatioMode.KeepAspectRatioByExpanding,
                                                          Qt.TransformationMode.SmoothTransformation)
        self.images[path] = image
        while len(self.images) > self.capacity:
            self.images.popitem(last=False)
        return image


def draw_sleeve(p, rect, image):
    if image is not None:
        p.drawImage(rect, image)
        return
    p.fillRect(rect, QColor('#1c1f23'))
    p.setPen(QPen(QColor('#30353b'), 1))
    p.setBrush(Qt.BrushStyle.NoBrush)
    p.drawEllipse(rect.center(), rect.width() * .32, rect.width() * .32)
    p.drawEllipse(rect.center(), rect.width() * .07, rect.width() * .07)


class TrackModel(QAbstractTableModel):
    """Tracks of the current view; more pages are fetched as the list scrolls."""
    COLUMNS = ('', 'TITLE', 'BPM', 'KEY', 'TIME')
    SORT_FOR = {1: 'Title', 2: 'BPM', 3: 'Key', 4: 'Time'}
    moreWanted = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.tracks = []
        self.total = 0
        self.loaded = {}  # audio path (casefolded) -> deck numbers it is on

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.tracks)

    def columnCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.COLUMNS)

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid() or not 0 <= index.row() < len(self.tracks):
            return None
        track = self.tracks[index.row()]
        if role == Qt.ItemDataRole.UserRole:
            return track
        if role == Qt.ItemDataRole.DisplayRole:
            return ('', track.title or 'Untitled', f'{track.bpm:.2f}' if track.bpm else '—',
                    track.key or '—', clock(track.duration))[index.column()]
        if role in (Qt.ItemDataRole.ToolTipRole, Qt.ItemDataRole.AccessibleTextRole):
            return f'{track.title} — {track.artist}'
        return None

    def headerData(self, section, orientation, role=Qt.ItemDataRole.DisplayRole):
        if orientation == Qt.Orientation.Horizontal and role == Qt.ItemDataRole.DisplayRole:
            return self.COLUMNS[section]
        return None

    def replace(self, tracks, total=None):
        self.beginResetModel()
        self.tracks = list(tracks)
        self.total = len(self.tracks) if total is None else total
        self.endResetModel()

    def append(self, tracks):
        if tracks:
            self.beginInsertRows(QModelIndex(), len(self.tracks), len(self.tracks) + len(tracks) - 1)
            self.tracks += tracks
            self.endInsertRows()

    def canFetchMore(self, parent=QModelIndex()):
        return not parent.isValid() and len(self.tracks) < self.total

    def fetchMore(self, parent=QModelIndex()):
        self.moreWanted.emit()

    def decks_for(self, track):
        return self.loaded.get(Path(track.path).as_posix().casefold(), ()) if track.path else ()


class TrackDelegate(QStyledItemDelegate):
    """Rows as a deck lists them: sleeve, title over artist, then tempo, key and length."""
    def __init__(self, artwork, parent=None):
        super().__init__(parent)
        self.artwork = artwork

    def paint(self, p, option, index):
        model = index.model()
        track = model.data(index, Qt.ItemDataRole.UserRole)
        if track is None:
            return
        r = QRectF(option.rect)
        p.save()
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        p.fillRect(r, QColor(BLUE) if selected else QColor('#121212' if index.row() % 2 else '#0c0c0c'))
        column = index.column()
        if column == 0:
            side = r.height() - 10
            draw_sleeve(p, QRectF(r.left() + 8, r.top() + 5, side, side), self.artwork.get(track.artwork))
        elif column == 1:
            decks = model.decks_for(track)
            right = r.right() - 8
            for deck in sorted(decks, reverse=True):
                box = QRectF(right - 26, r.center().y() - 13, 26, 26)
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(QColor('white' if selected else BLUE))
                p.drawRoundedRect(box, 3, 3)
                p.setPen(QColor(BLUE if selected else 'white'))
                p.setFont(font(16, True))
                p.drawText(box, Qt.AlignmentFlag.AlignCenter, str(deck))
                right = box.left() - 6
            text = QRectF(r.left() + 10, r.top(), right - r.left() - 14, r.height())
            p.setFont(font(r.height() * .32, True))
            p.setPen(QColor('white'))
            title = p.fontMetrics().elidedText(track.title or 'Untitled', Qt.TextElideMode.ElideRight, int(text.width()))
            p.drawText(QRectF(text.left(), text.top(), text.width(), text.height() * .5 - 1),
                       Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignBottom, title)
            p.setFont(font(r.height() * .26))
            p.setPen(QColor('#e8f1ff' if selected else '#9aa3ad'))
            artist = p.fontMetrics().elidedText(track.artist, Qt.TextElideMode.ElideRight, int(text.width()))
            p.drawText(QRectF(text.left(), text.top() + text.height() * .5 + 4, text.width(), text.height() * .4),
                       Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop, artist)
        elif column == 3 and track.key:
            p.setFont(font(r.height() * .3, True))
            w = p.fontMetrics().horizontalAdvance(track.key) + 18
            box = QRectF(r.center().x() - w / 2, r.center().y() - r.height() * .22, w, r.height() * .44)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor('#103a68' if selected else '#1f3a2a'))
            p.drawRoundedRect(box, 4, 4)
            p.setPen(QColor('white' if selected else '#7ee2a8'))
            p.drawText(box, Qt.AlignmentFlag.AlignCenter, track.key)
        else:
            p.setFont(font(r.height() * .32, column == 2))
            p.setPen(QColor('white' if selected or column == 2 else '#c7cdd4'))
            p.drawText(r, Qt.AlignmentFlag.AlignCenter, model.data(index))
        p.restore()

    def sizeHint(self, option, index):
        return QSize(80, 64)


class CategoryModel(QAbstractListModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.items = []

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.items)

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid() or not 0 <= index.row() < len(self.items):
            return None
        item = self.items[index.row()]
        if role == Qt.ItemDataRole.UserRole:
            return item
        if role in (Qt.ItemDataRole.DisplayRole, Qt.ItemDataRole.AccessibleTextRole):
            return item.name
        return None

    def replace(self, items):
        self.beginResetModel()
        self.items = list(items)
        self.endResetModel()


class CategoryDelegate(QStyledItemDelegate):
    def paint(self, p, option, index):
        item = index.data(Qt.ItemDataRole.UserRole)
        if item is None:
            return
        r = QRectF(option.rect)
        selected = bool(option.state & QStyle.StateFlag.State_Selected)
        p.save()
        p.fillRect(r, QColor(BLUE) if selected else QColor('#121212' if index.row() % 2 else '#0c0c0c'))
        icon = QRectF(r.left() + 18, r.center().y() - 14, 28, 28)
        p.setPen(QPen(QColor('white' if selected else '#8a929b'), 2))
        p.setBrush(Qt.BrushStyle.NoBrush)
        if item.folder:
            path = QPainterPath()
            path.moveTo(icon.left(), icon.top() + 6)
            path.lineTo(icon.left() + 10, icon.top() + 6)
            path.lineTo(icon.left() + 13, icon.top() + 10)
            path.lineTo(icon.right(), icon.top() + 10)
            path.lineTo(icon.right(), icon.bottom() - 3)
            path.lineTo(icon.left(), icon.bottom() - 3)
            path.closeSubpath()
            p.drawPath(path)
        else:
            for k in range(3):
                y = icon.top() + 7 + k * 7
                p.drawLine(QPointF(icon.left() + 7, y), QPointF(icon.right(), y))
                p.drawPoint(QPointF(icon.left() + 1, y))
        p.setFont(font(r.height() * .36))
        p.setPen(QColor('white'))
        right = r.right() - (34 if item.folder else 18)
        count = '' if item.folder else f'{item.count:,}'
        p.setFont(font(r.height() * .3))
        cw = p.fontMetrics().horizontalAdvance(count) + 12 if count else 0
        p.setPen(QColor('#e8f1ff' if selected else '#7d858e'))
        p.drawText(QRectF(right - cw, r.top(), cw, r.height()), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, count)
        p.setFont(font(r.height() * .36))
        p.setPen(QColor('white'))
        name = p.fontMetrics().elidedText(item.name, Qt.TextElideMode.ElideRight, int(right - cw - icon.right() - 30))
        p.drawText(QRectF(icon.right() + 18, r.top(), right - cw - icon.right() - 24, r.height()),
                   Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, name)
        if item.folder:
            p.setPen(QPen(QColor('#e8f1ff' if selected else '#7d858e'), 2.5))
            x, y = r.right() - 22, r.center().y()
            p.drawLine(QPointF(x - 6, y - 8), QPointF(x + 2, y))
            p.drawLine(QPointF(x + 2, y), QPointF(x - 6, y + 8))
        p.restore()

    def sizeHint(self, option, index):
        return QSize(80, 64)


class NavTile(QAbstractButton):
    """A category tile in the left column: a drawn icon over a small bold label."""
    def __init__(self, kind, label, parent=None):
        super().__init__(parent)
        self.kind = kind
        self.label = label
        self.on = False
        self.setFixedSize(76, 70)
        self.setAccessibleName(f'Browse by {label.lower()}')
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

    def set_on(self, on):
        if on != self.on:
            self.on = on
            self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(5, 3, -5, -3)
        if self.on:
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(BLUE))
            p.drawRoundedRect(r, 4, 4)
        ink = QColor('white' if self.on else '#b9bfc6')
        icon = QRectF(r.center().x() - 12, r.top() + 9, 24, 24)
        p.setPen(QPen(ink, 2))
        p.setBrush(Qt.BrushStyle.NoBrush)
        k = self.kind
        if k == 'PLAYLIST':
            for i in range(3):
                y = icon.top() + 5 + i * 7
                p.drawLine(QPointF(icon.left() + 6, y), QPointF(icon.right(), y))
                p.drawPoint(QPointF(icon.left(), y))
        elif k == 'TRACK':
            p.drawEllipse(QPointF(icon.left() + 6, icon.bottom() - 5), 5, 4)
            p.drawLine(QPointF(icon.left() + 11, icon.bottom() - 5), QPointF(icon.left() + 11, icon.top() + 1))
            p.drawLine(QPointF(icon.left() + 11, icon.top() + 1), QPointF(icon.right() - 2, icon.top() + 5))
        elif k == 'ARTIST':
            p.drawEllipse(QPointF(icon.center().x(), icon.top() + 7), 6, 6)
            p.drawArc(QRectF(icon.left() + 2, icon.top() + 15, 20, 18), 0, 180 * 16)
        elif k == 'ALBUM':
            p.drawEllipse(icon.center(), 11, 11)
            p.drawEllipse(icon.center(), 3, 3)
        elif k == 'KEY':
            p.drawEllipse(QPointF(icon.left() + 7, icon.center().y()), 6, 6)
            p.drawLine(QPointF(icon.left() + 13, icon.center().y()), QPointF(icon.right(), icon.center().y()))
            p.drawLine(QPointF(icon.right() - 4, icon.center().y()), QPointF(icon.right() - 4, icon.center().y() + 6))
            p.drawLine(QPointF(icon.right() - 9, icon.center().y()), QPointF(icon.right() - 9, icon.center().y() + 4))
        elif k == 'HISTORY':
            p.drawEllipse(icon.center(), 11, 11)
            p.drawLine(icon.center(), QPointF(icon.center().x(), icon.top() + 5))
            p.drawLine(icon.center(), QPointF(icon.center().x() + 6, icon.center().y() + 2))
        elif k == 'SEARCH':
            p.drawEllipse(QPointF(icon.left() + 9, icon.top() + 9), 8, 8)
            p.drawLine(QPointF(icon.left() + 15, icon.top() + 15), QPointF(icon.right() - 1, icon.bottom() - 1))
        p.setPen(ink)
        p.setFont(font(10, True))
        p.drawText(QRectF(r.left(), icon.bottom() + 5, r.width(), 14), Qt.AlignmentFlag.AlignCenter, self.label)
        p.end()


class SourceTile(QAbstractButton):
    """The navy source square heading the left column: which library is being browsed."""
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setFixedSize(76, 88)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAccessibleName('Choose a music source')
        self.kind, self.name = 'COLL', 'Collection'

    def set_source(self, label, path):
        self.kind, self.name = ('USB' if path else 'COLL'), label
        self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(5, 5, -5, -5)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor('#1b3a6a'))
        p.drawRoundedRect(r, 4, 4)
        p.setPen(QColor('white'))
        p.setFont(font(16, True))
        p.drawText(QRectF(r.left(), r.top() + 14, r.width(), 24), Qt.AlignmentFlag.AlignCenter, self.kind)
        p.setFont(font(10))
        name = p.fontMetrics().elidedText(self.name, Qt.TextElideMode.ElideRight, int(r.width() - 8))
        p.drawText(QRectF(r.left() + 4, r.bottom() - 24, r.width() - 8, 16), Qt.AlignmentFlag.AlignCenter, name)
        p.end()


class TouchKeyboard(QWidget):
    def __init__(self, target, parent=None):
        super().__init__(parent)
        self.setObjectName('keyboard')
        self.target = target
        self.shift = False
        self.letters = []
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(6)
        for chars in ('1234567890', 'qwertyuiop', 'asdfghjkl', 'zxcvbnm'):
            row = QHBoxLayout()
            row.setSpacing(6)
            for char in chars:
                key = self.key(char.upper(), lambda checked=False, c=char: self.insert(c.upper() if self.shift else c))
                self.letters.append((key, char))
                row.addWidget(key)
            layout.addLayout(row)
        row = QHBoxLayout()
        shift = self.key('SHIFT', self.toggle_shift)
        shift.setCheckable(True)
        row.addWidget(shift)
        for text in ('-', "'", '/', '.'):
            row.addWidget(self.key(text, lambda checked=False, c=text: self.insert(c)))
        row.addWidget(self.key('SPACE', lambda: self.insert(' ')), 3)
        delete = self.key('DELETE', target.backspace)
        delete.setAutoRepeat(True)
        row.addWidget(delete)
        row.addWidget(self.key('CLEAR', target.clear))
        row.addWidget(self.key('DONE', self.hide))
        layout.addLayout(row)
        self.hide()

    def key(self, text, action):
        button = QPushButton(text)
        button.setMinimumHeight(52)
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        button.clicked.connect(action)
        return button

    def insert(self, text):
        self.target.insert(text)

    def toggle_shift(self):
        self.shift = not self.shift
        for key, char in self.letters:
            key.setText(char.upper() if self.shift else char)


class SourceCard(QAbstractButton):
    """A library on the SOURCE screen with what is on it."""
    def __init__(self, label, path, parent=None):
        super().__init__(parent)
        self.label, self.path, self.stats = label, path, None
        self.setMinimumSize(300, 190)
        self.setMaximumSize(720, 230)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setAccessibleName(f'Open {label}')

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(6, 6, -6, -6)
        p.setPen(QPen(QColor('#2a2e33'), 1))
        p.setBrush(QColor('#141619' if not self.isDown() else '#1d2733'))
        p.drawRoundedRect(r, 6, 6)
        tile = QRectF(r.left() + 18, r.top() + 18, 70, 70)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor('#1b3a6a'))
        p.drawRoundedRect(tile, 4, 4)
        p.setPen(QColor('white'))
        p.setFont(font(18, True))
        p.drawText(tile, Qt.AlignmentFlag.AlignCenter, 'USB' if self.path else 'COLL')
        p.setFont(font(24, True))
        p.drawText(QRectF(tile.right() + 18, tile.top(), r.width() - 130, 38), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   p.fontMetrics().elidedText(self.label, Qt.TextElideMode.ElideRight, int(r.width() - 130)))
        p.setFont(font(15))
        p.setPen(QColor('#9aa3ad'))
        p.drawText(QRectF(tile.right() + 18, tile.top() + 38, r.width() - 130, 26), Qt.AlignmentFlag.AlignLeft,
                   'Rekordbox collection' if not self.path else 'Rekordbox USB export')
        rows = []
        if self.stats:
            rows = [('Songs', f"{self.stats['tracks']:,}"), ('Playlists', f"{self.stats['playlists']:,}")]
            if 'total' in self.stats:
                rows.append(('Available', f"{self.stats['free'] / 1e9:.1f} of {self.stats['total'] / 1e9:.0f} GB"))
        y = tile.bottom() + 16
        for key, value in rows:
            p.setPen(QColor('#7d858e'))
            p.setFont(font(15))
            p.drawText(QRectF(r.left() + 18, y, 120, 24), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, key)
            p.setPen(QColor('white'))
            p.drawText(QRectF(r.left() + 140, y, r.width() - 160, 24), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, value)
            y += 26
        p.end()


class DeckButton(QAbstractButton):
    """LOAD over the deck's number, as large as a thumb; dimmed while that deck can't take a track."""
    def __init__(self, deck, parent=None):
        super().__init__(parent)
        self.deck = deck
        self.locked = False
        self.setFixedSize(104, 80)
        self.setAccessibleName(f'Load the selected track on deck {deck + 1}')

    def set_locked(self, locked):
        if locked != self.locked:
            self.locked = locked
            self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        r = QRectF(self.rect()).adjusted(1, 1, -1, -1)
        live = self.isEnabled() and not self.locked
        fill = BLUE if self.isDown() and live else ('#1b1b1b' if live else '#101010')
        p.setPen(QPen(QColor('#4a5058' if live else '#24272b'), 1))
        p.setBrush(QColor(fill))
        p.drawRoundedRect(r, 5, 5)
        ink = QColor('white' if live else '#4a4f55')
        p.setPen(ink)
        p.setFont(font(12, True))
        p.drawText(QRectF(r.left(), r.top() + 6, r.width(), 16), Qt.AlignmentFlag.AlignCenter, 'LOAD')
        p.setFont(font(36, True))
        p.drawText(QRectF(r.left(), r.top() + 20, r.width(), r.height() - 22), Qt.AlignmentFlag.AlignCenter, str(self.deck + 1))
        p.end()


class LoadBar(QFrame):
    """The selected track and the decks it can go to."""
    def __init__(self, artwork, parent=None):
        super().__init__(parent)
        self.setObjectName('loadbar')
        self.artwork = artwork
        self.track = None
        self.setFixedHeight(104)

    def set_track(self, track):
        self.track = track
        self.update()

    def paintEvent(self, event):
        super().paintEvent(event)
        if self.track is None:
            return
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
        r = QRectF(self.rect())
        side = r.height() - 22
        draw_sleeve(p, QRectF(14, 11, side, side), self.artwork.get(self.track.artwork))
        x = 14 + side + 16
        width = max(40.0, self.info_width - x)
        p.setPen(QColor('white'))
        p.setFont(font(21, True))
        p.drawText(QRectF(x, 12, width, 30), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   p.fontMetrics().elidedText(self.track.title, Qt.TextElideMode.ElideRight, int(width)))
        p.setPen(QColor('#9aa3ad'))
        p.setFont(font(17))
        p.drawText(QRectF(x, 42, width, 24), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter,
                   p.fontMetrics().elidedText(self.track.artist, Qt.TextElideMode.ElideRight, int(width)))
        p.setPen(QColor('#d6dade'))
        p.setFont(font(15, True))
        detail = '   '.join(filter(None, (f'{self.track.bpm:.2f} BPM' if self.track.bpm else '', self.track.key,
                                          clock(self.track.duration))))
        p.drawText(QRectF(x, 68, width, 22), Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter, detail)
        p.end()

    @property
    def info_width(self):
        return self.layout().itemAt(1).geometry().left() - 16 if self.layout() and self.layout().count() > 1 else self.width() / 2


class BrowserPage(QWidget):
    loadRequested = Signal(int, object)  # deck index, CatalogTrack

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName('browser')
        self.worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix='collection-browser')
        # Finding sticks and reading their space has its own thread: a slow drive never holds up the list.
        self.devices = ThreadPoolExecutor(max_workers=1, thread_name_prefix='browser-devices')
        self.artwork = Artwork()
        self.future = None          # the running track query
        self.side = []              # (future, callback) for sources, stats and lists
        self.revision = 0
        self.running_revision = -1
        self.appending = False
        self.closed = False
        self.source_label, self.source_path = 'Collection', ''
        self.kind = 'TRACK'
        self.trail = []             # (title, view) pairs above the current view, for BACK
        self.view = ('tracks', None)  # ('sources',) | ('list', kind, parent) | ('tracks', scope)
        self.title = 'TRACK'
        self.sort = ('Title', False)
        self.deck_order = None
        self.unavailable = ''
        self.locked = {}

        outer = QHBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        nav = QFrame()
        nav.setObjectName('browserNav')
        column = QVBoxLayout(nav)
        column.setContentsMargins(0, 2, 0, 2)
        column.setSpacing(2)
        self.source_tile = SourceTile()
        self.source_tile.clicked.connect(self.show_sources)
        column.addWidget(self.source_tile)
        self.tiles = {}
        for kind, label in NAV:
            tile = NavTile(kind, label)
            tile.clicked.connect(lambda checked=False, k=kind: self.open_category(k))
            self.tiles[kind] = tile
            column.addWidget(tile)
        column.addStretch()
        outer.addWidget(nav)

        main = QVBoxLayout()
        main.setContentsMargins(0, 0, 0, 0)
        main.setSpacing(0)
        outer.addLayout(main, 1)
        top = QFrame()
        top.setObjectName('browserTop')
        bar = QHBoxLayout(top)
        bar.setContentsMargins(14, 8, 14, 8)
        bar.setSpacing(12)
        self.back = QPushButton('‹')
        self.back.setObjectName('back')
        self.back.setAccessibleName('Back')
        self.back.setFixedSize(52, 52)
        self.back.clicked.connect(self.go_back)
        bar.addWidget(self.back)
        self.heading = QLabel('TRACK')
        self.heading.setObjectName('heading')
        bar.addWidget(self.heading, 1)
        self.search = QLineEdit()
        self.search.setPlaceholderText('Search title or artist')
        self.search.setAccessibleName('Search collection')
        self.search.setClearButtonEnabled(True)
        self.search.setMinimumHeight(52)
        self.search.setMinimumWidth(260)
        bar.addWidget(self.search, 1)
        keyboard = QPushButton('ABC')
        keyboard.setObjectName('keys')
        keyboard.setAccessibleName('Show the on-screen keyboard')
        keyboard.setFixedSize(60, 52)
        bar.addWidget(keyboard)
        self.refresh = QPushButton('↻')
        self.refresh.setObjectName('keys')
        self.refresh.setAccessibleName('Look for USB sticks and reload')
        self.refresh.setFixedSize(60, 52)
        bar.addWidget(self.refresh)
        main.addWidget(top)

        self.stack = QStackedWidget()
        main.addWidget(self.stack, 1)
        # SOURCE screen
        self.sources_page = QWidget()
        self.sources_page.setObjectName('sourcesPage')
        self.sources_grid = QGridLayout(self.sources_page)
        self.sources_grid.setContentsMargins(24, 24, 24, 24)
        self.sources_grid.setSpacing(16)
        self.stack.addWidget(self.sources_page)
        # Category lists
        self.categories = CategoryModel(self)
        self.list = QListView()
        self.list.setModel(self.categories)
        self.list.setItemDelegate(CategoryDelegate(self.list))
        self.list.setUniformItemSizes(True)
        self.list.setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.list.setAccessibleName('Browse list')
        self.list.clicked.connect(self.open_item)
        QScroller.grabGesture(self.list.viewport(), QScroller.ScrollerGestureType.TouchGesture)
        self.stack.addWidget(self.list)
        # Tracks
        self.model = TrackModel(self)
        self.model.moreWanted.connect(self.fetch_more)
        self.table = QTableView()
        self.table.setAccessibleName('Tracks; tap a heading to sort')
        self.table.setModel(self.model)
        self.table.setItemDelegate(TrackDelegate(self.artwork, self.table))
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.table.setHorizontalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.table.setShowGrid(False)
        self.table.setWordWrap(False)
        self.table.verticalHeader().hide()
        self.table.verticalHeader().setDefaultSectionSize(64)
        header = self.table.horizontalHeader()
        header.setMinimumHeight(40)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Fixed)
        self.table.setColumnWidth(0, 72)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        for col, width in ((2, 110), (3, 100), (4, 96)):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.Fixed)
            self.table.setColumnWidth(col, width)
        header.setSectionsClickable(True)
        header.setSortIndicator(1, Qt.SortOrder.AscendingOrder)
        header.setSortIndicatorShown(True)
        header.sectionClicked.connect(self.sort_by)
        QScroller.grabGesture(self.table.viewport(), QScroller.ScrollerGestureType.TouchGesture)
        self.table.selectionModel().selectionChanged.connect(lambda *_: self.selection_changed())
        self.stack.addWidget(self.table)

        self.keyboard = TouchKeyboard(self.search)
        main.addWidget(self.keyboard)
        keyboard.clicked.connect(lambda: self.keyboard.setVisible(not self.keyboard.isVisible()))
        self.message = QLabel('Opening collection…')
        self.message.setObjectName('message')
        main.addWidget(self.message)
        self.load_bar = LoadBar(self.artwork)
        loads = QHBoxLayout(self.load_bar)
        loads.setContentsMargins(14, 12, 14, 12)
        loads.setSpacing(10)
        loads.addStretch(1)
        self.deck_row = QHBoxLayout()
        self.deck_row.setSpacing(10)
        loads.addLayout(self.deck_row)
        self.deck_buttons = {}
        for idx in range(4):
            button = DeckButton(idx)
            button.clicked.connect(lambda checked=False, i=idx: self.load(i))
            self.deck_buttons[idx] = button
        main.addWidget(self.load_bar)
        self.availability = self.message  # the one line for load results and reasons

        self.debounce = QTimer(self)
        self.debounce.setSingleShot(True)
        self.debounce.setInterval(180)
        self.debounce.timeout.connect(self.start_query)
        self.poll = QTimer(self)
        self.poll.setInterval(40)
        self.poll.timeout.connect(self.complete_query)
        self.side_poll = QTimer(self)
        self.side_poll.setInterval(40)
        self.side_poll.timeout.connect(self.complete_side)
        self.search.textChanged.connect(self.search_changed)
        self.refresh.clicked.connect(self.show_sources)
        self.open_category('TRACK')
        self.run(sources, self.sources_ready)

    # Worker plumbing -----------------------------------------------------------------------
    def run(self, function, callback, *args):
        if self.closed:
            return
        pool = self.devices if function in (sources, source_stats) else self.worker
        self.side.append((pool.submit(function, *args), callback))
        self.side_poll.start()

    def complete_side(self):
        for item in [s for s in self.side if s[0].done()]:
            self.side.remove(item)
            future, callback = item
            try:
                callback(future.result())
            except Exception as exc:
                self.message.setText(f'Library unavailable: {exc}. Start Rekordbox or reconnect the USB, then tap ↻.')
        if not self.side:
            self.side_poll.stop()

    # Navigation ---------------------------------------------------------------------------
    def set_heading(self):
        crumbs = [self.source_label.upper()] + [title for title, _ in self.trail] + [self.title]
        self.heading.setText('  ›  '.join(crumbs))
        self.back.setEnabled(bool(self.trail) or self.view[0] != 'sources')
        for kind, tile in self.tiles.items():
            tile.set_on(kind == self.kind and self.view[0] != 'sources')

    def navigate(self, view, title):
        self.view, self.title = view, title
        self.set_heading()
        if view[0] == 'sources':
            self.stack.setCurrentWidget(self.sources_page)
            self.run(sources, self.sources_ready)
        elif view[0] == 'list':
            self.stack.setCurrentWidget(self.list)
            self.categories.replace([])
            self.message.setText('Opening…')
            kind, parent = view[1], view[2]
            self.run(categories, lambda items, v=view: self.list_ready(v, items), self.source_path, kind, parent)
        else:
            self.stack.setCurrentWidget(self.table)
            ordered = bool(view[1]) and view[1][0] in ('PLAYLIST', 'HISTORY')
            self.sort = (ORDER, False) if ordered else ('Title', False)
            # A playlist plays in its own order until a heading is tapped.
            self.table.horizontalHeader().setSortIndicatorShown(not ordered)
            self.table.horizontalHeader().setSortIndicator(1, Qt.SortOrder.AscendingOrder)
            self.request()

    def open_category(self, kind):
        self.kind = kind
        self.trail = []
        if kind in ('TRACK', 'SEARCH'):
            self.navigate(('tracks', None), kind)
            if kind == 'SEARCH':
                self.search.setFocus()
                self.keyboard.show()
        else:
            self.navigate(('list', kind, None), kind)

    def open_item(self, index):
        item = index.data(Qt.ItemDataRole.UserRole)
        if item is None:
            return
        self.trail.append((self.title, self.view))
        kind = self.view[1]
        if item.folder:
            self.navigate(('list', kind, item.identifier), item.name)
        else:
            self.navigate(('tracks', (kind, item.identifier)), item.name)

    def go_back(self):
        """One level up; False at a category's root, where BACK leaves the browser."""
        if self.trail:
            title, view = self.trail.pop()
            self.navigate(view, title)
            return True
        if self.view[0] == 'sources':
            self.open_category(self.kind)
            return True
        return False

    def show_sources(self):
        self.trail = []
        self.navigate(('sources',), 'SOURCE')

    def sources_ready(self, available):
        while self.sources_grid.count():
            widget = self.sources_grid.takeAt(0).widget()
            if widget:
                widget.deleteLater()
        for n, (label, path) in enumerate(available):
            card = SourceCard(label, path)
            card.clicked.connect(lambda checked=False, l=label, p=path: self.choose_source(l, p))
            self.sources_grid.addWidget(card, n // 2, n % 2, Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignTop)
            self.run(source_stats, lambda stats, c=card: (setattr(c, 'stats', stats), c.update()), path)
        self.sources_grid.setRowStretch(len(available) // 2 + 1, 1)
        if self.view[0] == 'sources':
            self.message.setText('Tap a source to browse it.' if len(available) > 1 else
                                 'No USB stick found. Plug one in and tap ↻, or browse the collection.')
        if self.source_path and self.source_path not in [p for _, p in available]:
            self.choose_source('Collection', '')  # the stick being browsed was removed

    def choose_source(self, label, path):
        self.source_label, self.source_path = label, path
        self.source_tile.set_source(label, path)
        self.open_category('PLAYLIST')

    def list_ready(self, view, items):
        if view != self.view:
            return
        self.categories.replace(items)
        self.message.setText(f'{len(items):,} {self.title.lower()}' if items else 'Nothing here')

    # Track queries ---------------------------------------------------------------------------
    def scope(self):
        return self.view[1] if self.view[0] == 'tracks' else None

    def search_changed(self):
        if self.view[0] != 'tracks':
            self.kind, self.trail = 'SEARCH', []
            self.view, self.title = ('tracks', None), 'SEARCH'
            self.stack.setCurrentWidget(self.table)
            self.set_heading()
        self.request()

    def sort_by(self, column):
        field = TrackModel.SORT_FOR.get(column)
        if not field:
            return
        self.sort = (field, not self.sort[1] if self.sort[0] == field else False)
        self.table.horizontalHeader().setSortIndicatorShown(True)
        self.table.horizontalHeader().setSortIndicator(column, Qt.SortOrder.DescendingOrder if self.sort[1]
                                                       else Qt.SortOrder.AscendingOrder)
        self.request()

    def request(self, *args):
        self.revision += 1
        self.appending = False
        # Never leave a row from an older query selected while the new query is pending.
        self.table.clearSelection()
        self.debounce.start()

    def fetch_more(self):
        if self.future or self.closed or self.debounce.isActive():
            return
        self.appending = True
        self.start_query()

    def start_query(self):
        if self.closed or self.future:
            return
        self.running_revision = self.revision
        offset = len(self.model.tracks) if self.appending else 0
        args = (self.source_path, self.search.text(), self.sort[0], self.sort[1], offset, PAGE, self.scope())
        if not self.appending:
            self.message.setText('Searching…')
        self.future = self.worker.submit(browse, *args)
        self.poll.start()

    def complete_query(self):
        if not self.future or not self.future.done():
            return
        future, self.future = self.future, None
        self.poll.stop()
        if self.running_revision != self.revision:
            self.start_query()
            return
        try:
            total, tracks = future.result()
        except Exception as exc:
            self.model.replace([])
            self.message.setText(f'Library unavailable: {exc}. Start Rekordbox or reconnect the USB, then tap ↻.')
            return
        if self.appending:
            self.model.append(tracks)
        else:
            self.model.replace(tracks, total)
            self.table.scrollToTop()
        self.appending = False
        if self.message.text() in ('Searching…', 'Opening…') or not self.message.text() or self.view[0] == 'tracks':
            self.message.setText(f'{total:,} tracks' if total else 'No matching tracks')

    # Loading ------------------------------------------------------------------------------------
    def selected_track(self):
        rows = self.table.selectionModel().selectedRows()
        return self.model.tracks[rows[0].row()] if rows and rows[0].row() < len(self.model.tracks) else None

    def selection_changed(self):
        self.load_bar.set_track(self.selected_track())
        self.sync_decks()

    def load(self, idx):
        track = self.selected_track()
        if track is None:
            self.message.setText('Tap a track first.')
        elif self.source_path:
            self.message.setText('Loading straight from a USB stick is not supported yet: '
                                 'load it with the controller, or pick it from the Collection.')
        elif self.locked.get(idx):
            self.message.setText(self.locked[idx])
        else:
            self.loadRequested.emit(idx, track)

    def set_decks(self, order, locked, unavailable='', loaded=None):
        """LOAD buttons in physical deck order; ``locked`` maps a deck to why it can't load;
        ``loaded`` maps each deck to the file it holds, for the badges in the list."""
        if order != self.deck_order:
            self.deck_order = order
            for button in self.deck_buttons.values():
                self.deck_row.removeWidget(button)
                button.hide()
            for idx in order:
                self.deck_row.addWidget(self.deck_buttons[idx])
                self.deck_buttons[idx].show()
        self.locked = {idx: unavailable or why for idx, why in locked.items()}
        if unavailable != self.unavailable:
            self.unavailable = unavailable
            if unavailable:
                self.message.setText(unavailable)
        if loaded is not None:
            index = {}
            for deck, path in loaded.items():
                if path:
                    index.setdefault(Path(path).as_posix().casefold(), []).append(deck + 1)
            if index != self.model.loaded:
                self.model.loaded = index
                self.table.viewport().update()
        self.sync_decks()

    def show_load_status(self, text):
        self.message.setText(text)

    def sync_decks(self):
        # A locked deck stays pressable: on a touch screen, pressing it is how its reason is seen.
        selected = self.selected_track() is not None
        for idx, button in self.deck_buttons.items():
            why = self.locked.get(idx, '')
            button.setEnabled(selected)
            button.setToolTip(why)
            button.set_locked(bool(why))

    def shutdown(self):
        self.closed = True
        # Release Qt's shared gesture targets before their viewports are destroyed.
        QScroller.ungrabGesture(self.table.viewport())
        QScroller.ungrabGesture(self.list.viewport())
        self.debounce.stop()
        self.poll.stop()
        self.side_poll.stop()
        self.worker.shutdown(wait=False, cancel_futures=True)
        self.devices.shutdown(wait=False, cancel_futures=True)
