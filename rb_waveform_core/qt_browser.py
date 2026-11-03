"""Touch collection browser, sharing the main window without blocking playback."""
from concurrent.futures import ThreadPoolExecutor
from PySide6.QtCore import Qt, QTimer, QAbstractTableModel, QModelIndex
from PySide6.QtWidgets import (QWidget, QVBoxLayout, QHBoxLayout, QPushButton,
    QLineEdit, QLabel, QComboBox, QTableView, QHeaderView, QAbstractItemView, QScroller)
from .catalog import SORTS, sources, browse


class TrackModel(QAbstractTableModel):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.tracks = []

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.tracks)

    def columnCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(SORTS)

    def data(self, index, role=Qt.ItemDataRole.DisplayRole):
        if not index.isValid() or not 0 <= index.row() < len(self.tracks):
            return None
        track = self.tracks[index.row()]
        if role == Qt.ItemDataRole.DisplayRole:
            return (track.title or 'Untitled', track.artist, f'{track.bpm:.2f}' if track.bpm else '—',
                    track.key or '—', f'{int(track.duration)//60:02}:{int(track.duration)%60:02}')[index.column()]
        if role == Qt.ItemDataRole.TextAlignmentRole and index.column() >= 2:
            return int(Qt.AlignmentFlag.AlignCenter)
        if role == Qt.ItemDataRole.ToolTipRole:
            return track.title + '\n' + track.artist
        return None

    def headerData(self, section, orientation, role=Qt.ItemDataRole.DisplayRole):
        if orientation == Qt.Orientation.Horizontal and role == Qt.ItemDataRole.DisplayRole:
            return SORTS[section].upper()
        return None

    def replace(self, tracks):
        self.beginResetModel()
        self.tracks = tracks
        self.endResetModel()


class TouchKeyboard(QWidget):
    def __init__(self, target, parent=None):
        super().__init__(parent)
        self.target = target
        self.shift = False
        self.letters = []
        layout = QVBoxLayout(self)
        layout.setContentsMargins(6, 8, 6, 8)
        layout.setSpacing(5)
        for chars in ('1234567890', 'qwertyuiop', 'asdfghjkl', 'zxcvbnm'):
            row = QHBoxLayout()
            row.setSpacing(5)
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
        button.setMinimumHeight(48)
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        button.clicked.connect(action)
        return button

    def insert(self, text):
        self.target.insert(text)

    def toggle_shift(self):
        self.shift = not self.shift
        for key, char in self.letters:
            key.setText(char.upper() if self.shift else char)


class BrowserPage(QWidget):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName('browser')
        self.worker = ThreadPoolExecutor(max_workers=1, thread_name_prefix='collection-browser')
        self.future = None
        self.sources_future = None
        self.revision = 0
        self.running_revision = -1
        self.offset = 0
        self.total = 0
        self.closed = False
        layout = QVBoxLayout(self)
        layout.setContentsMargins(18, 16, 18, 16)
        layout.setSpacing(12)
        tools = QHBoxLayout()
        self.source = QComboBox()
        self.source.setAccessibleName('Music source')
        self.source.addItem('Collection', '')
        tools.addWidget(self.source)
        self.search = QLineEdit()
        self.search.setPlaceholderText('Search title or artist')
        self.search.setAccessibleName('Search collection')
        self.search.setMinimumHeight(48)
        tools.addWidget(self.search, 1)
        keyboard = QPushButton('KEYBOARD')
        keyboard.setMinimumHeight(48)
        tools.addWidget(keyboard)
        self.refresh = QPushButton('REFRESH')
        tools.addWidget(self.refresh)
        layout.addLayout(tools)
        self.model = TrackModel(self)
        self.table = QTableView()
        self.table.setAccessibleName('Collection tracks; tap a column heading to sort')
        self.table.setModel(self.model)
        self.table.setSelectionBehavior(QAbstractItemView.SelectionBehavior.SelectRows)
        self.table.setSelectionMode(QAbstractItemView.SelectionMode.SingleSelection)
        self.table.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)
        self.table.setVerticalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.table.setHorizontalScrollMode(QAbstractItemView.ScrollMode.ScrollPerPixel)
        self.table.setShowGrid(False)
        self.table.setWordWrap(False)
        self.table.setTextElideMode(Qt.TextElideMode.ElideRight)
        self.table.setAlternatingRowColors(True)
        self.table.verticalHeader().hide()
        self.table.verticalHeader().setDefaultSectionSize(54)
        header = self.table.horizontalHeader()
        header.setMinimumHeight(52)
        header.setSectionResizeMode(0, QHeaderView.ResizeMode.Stretch)
        header.setSectionResizeMode(1, QHeaderView.ResizeMode.Stretch)
        for col, width in ((2,110),(3,90),(4,100)):
            header.setSectionResizeMode(col, QHeaderView.ResizeMode.Fixed)
            self.table.setColumnWidth(col,width)
        header.setSortIndicatorShown(True)
        header.setSectionsClickable(True)
        header.setSortIndicator(0, Qt.SortOrder.AscendingOrder)
        self.table.setSortingEnabled(True)
        header.sortIndicatorChanged.connect(self.request)
        QScroller.grabGesture(self.table.viewport(), QScroller.ScrollerGestureType.TouchGesture)
        layout.addWidget(self.table, 1)
        bottom = QHBoxLayout()
        self.previous = QPushButton('PREVIOUS')
        self.next = QPushButton('NEXT')
        self.previous.clicked.connect(lambda: self.page(-1))
        self.next.clicked.connect(lambda: self.page(1))
        self.message = QLabel('Opening collection…')
        self.message.setWordWrap(True)
        bottom.addWidget(self.previous)
        bottom.addWidget(self.message,1)
        bottom.addWidget(self.next)
        layout.addLayout(bottom)
        self.availability = QLabel('Browsing is read-only. Direct deck loading is not connected in this build.')
        self.availability.setWordWrap(True)
        layout.addWidget(self.availability)
        self.keyboard = TouchKeyboard(self.search)
        layout.addWidget(self.keyboard)
        keyboard.clicked.connect(lambda: self.keyboard.setVisible(not self.keyboard.isVisible()))
        self.debounce = QTimer(self)
        self.debounce.setSingleShot(True)
        self.debounce.setInterval(180)
        self.debounce.timeout.connect(self.start_query)
        self.poll = QTimer(self)
        self.poll.setInterval(50)
        self.poll.timeout.connect(self.complete_query)
        self.source_poll = QTimer(self)
        self.source_poll.setInterval(50)
        self.source_poll.timeout.connect(self.complete_sources)
        self.source.currentIndexChanged.connect(self.request)
        self.search.textChanged.connect(self.request)
        self.refresh.clicked.connect(self.refresh_sources)
        self.refresh_sources()

    def refresh_sources(self):
        if self.sources_future or self.closed:
            return
        self.refresh.setEnabled(False)
        self.sources_future = self.worker.submit(sources)
        self.source_poll.start()

    def complete_sources(self):
        if not self.sources_future or not self.sources_future.done():
            return
        future, self.sources_future = self.sources_future, None
        self.source_poll.stop()
        self.refresh.setEnabled(True)
        try:
            available = future.result()
        except Exception as exc:
            self.message.setText(f'Sources unavailable: {exc}. Tap REFRESH to retry.')
            return
        selected = self.source.currentData()
        self.source.blockSignals(True)
        self.source.clear()
        for label, path in available:
            self.source.addItem(label, path)
        self.source.setCurrentIndex(max(0,self.source.findData(selected)))
        self.source.blockSignals(False)
        self.request()

    def request(self, *args, reset=True):
        if reset:
            self.offset = 0
        self.revision += 1
        # Never leave a row from an older query selected while the new query is pending.
        self.table.clearSelection()
        self.previous.setEnabled(False)
        self.next.setEnabled(False)
        self.debounce.start()

    def page(self, delta):
        self.offset = max(0, self.offset + delta*200)
        self.request(reset=False)

    def start_query(self):
        if self.closed or self.future:
            return
        self.running_revision = self.revision
        header = self.table.horizontalHeader()
        args = (self.source.currentData(), self.search.text(), SORTS[header.sortIndicatorSection()],
                header.sortIndicatorOrder() == Qt.SortOrder.DescendingOrder, self.offset)
        self.message.setText('Searching collection…')
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
            self.total, tracks = future.result()
            self.model.replace(tracks)
            self.table.scrollToTop()
            self.message.setText(f'{self.offset+1}–{self.offset+len(tracks)} of {self.total:,} tracks' if tracks else 'No matching tracks')
            self.previous.setEnabled(self.offset > 0)
            self.next.setEnabled(self.offset + len(tracks) < self.total)
        except Exception as exc:
            self.model.replace([])
            self.message.setText(f'Collection unavailable: {exc}. Start Rekordbox or reconnect the USB, then tap REFRESH.')

    def shutdown(self):
        self.closed = True
        # Release Qt's shared gesture target before its viewport is destroyed.
        QScroller.ungrabGesture(self.table.viewport())
        self.debounce.stop()
        self.poll.stop()
        self.source_poll.stop()
        self.worker.shutdown(wait=False, cancel_futures=True)
