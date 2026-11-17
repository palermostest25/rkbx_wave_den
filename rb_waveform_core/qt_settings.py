"""Wave's settings page: touch-sized switches, segmented choices and dropdowns, one line of help each."""
from __future__ import annotations

import math
import sys

from PySide6.QtCore import Qt, QRectF, QSize, QUrl, QVariantAnimation, QEasingCurve, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QDesktopServices
from PySide6.QtWidgets import (QAbstractButton, QComboBox, QFrame, QHBoxLayout, QLabel, QListView, QListWidget,
                               QProgressBar, QPushButton, QScrollArea, QStackedWidget, QVBoxLayout, QWidget,
                               QDoubleSpinBox)

from . import __version__

ACCENT = '#1a79eb'
REPO = 'https://github.com/palermostest25/rkbx_wave_den'


class Switch(QAbstractButton):
    """An on/off switch, as on a phone: the whole row is a finger-sized target."""
    def __init__(self, checked=False, parent=None):
        super().__init__(parent)
        self.setCheckable(True)
        self.setChecked(checked)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self._knob = 1.0 if checked else 0.0
        self._slide = QVariantAnimation(self, duration=140, easingCurve=QEasingCurve.Type.OutCubic)
        self._slide.valueChanged.connect(self._moved)
        self.toggled.connect(self._animate)

    def sizeHint(self):
        return QSize(62, 36)

    def _moved(self, value):
        self._knob = float(value)
        self.update()

    def _animate(self, on):
        self._slide.stop()
        self._slide.setStartValue(self._knob)
        self._slide.setEndValue(1.0 if on else 0.0)
        self._slide.start()

    def setChecked(self, on):
        super().setChecked(on)
        if self.signalsBlocked() or not hasattr(self, '_slide'):
            self._knob = 1.0 if on else 0.0  # set quietly, as when Wave updates it: no animation
            self.update()

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        track = QRectF(1, 1, self.width() - 2, self.height() - 2)
        off, on = QColor('#3a3f46'), QColor(ACCENT if self.isEnabled() else '#2a4466')
        mix = QColor(round(off.red() + (on.red() - off.red()) * self._knob), round(off.green() + (on.green() - off.green()) * self._knob),
                     round(off.blue() + (on.blue() - off.blue()) * self._knob))
        p.setPen(QPen(QColor('#7fb5ff'), 2) if self.hasFocus() else Qt.PenStyle.NoPen)
        p.setBrush(mix)
        p.drawRoundedRect(track, track.height() / 2, track.height() / 2)
        d = track.height() - 6
        x = track.left() + 3 + (track.width() - d - 6) * self._knob
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor('#ffffff' if self.isEnabled() else '#9aa3ad'))
        p.drawEllipse(QRectF(x, track.top() + 3, d, d))


class Segmented(QFrame):
    """A row of choices, one lit: for two to five options, quicker than a dropdown on a touch screen."""
    changed = Signal(object)

    def __init__(self, options, value, parent=None):
        super().__init__(parent)
        self.setObjectName('segment')
        row = QHBoxLayout(self)
        row.setContentsMargins(3, 3, 3, 3)
        row.setSpacing(2)
        self.buttons = []
        for text, data in options:
            button = QPushButton(text)
            button.setCheckable(True)
            button.setCursor(Qt.CursorShape.PointingHandCursor)
            button.clicked.connect(lambda _=False, d=data: self.set_value(d, emit=True))
            row.addWidget(button)
            self.buttons.append((button, data))
        self.set_value(value)

    def value(self):
        return next((d for b, d in self.buttons if b.isChecked()), None)

    def set_value(self, value, emit=False):
        def distance(data):
            if isinstance(value, (int, float)) and not isinstance(value, bool) and isinstance(data, (int, float)):
                return abs(data - value)  # numbers: the nearest option, for values set before these choices
            return 0 if data == value else 1
        best = min(self.buttons, key=lambda bd: distance(bd[1]))
        for button, data in self.buttons:
            button.setChecked(button is best[0])
            button.setProperty('selected', button is best[0])
            button.style().unpolish(button)
            button.style().polish(button)
        if emit:
            self.changed.emit(best[1])


def combo(options=(), value=None, accessible=''):
    """A dropdown whose list is styled like the rest of Wave (Qt's own list view takes the stylesheet)."""
    box = QComboBox()
    box.setView(QListView())
    box.view().setVerticalScrollMode(QListView.ScrollMode.ScrollPerPixel)
    box.setCursor(Qt.CursorShape.PointingHandCursor)
    box.setAccessibleName(accessible)
    box.setMinimumWidth(320)
    box.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
    for text, data in options:
        box.addItem(text, data)
    if value is not None:
        index = box.findData(value)
        box.setCurrentIndex(max(0, index))
    return box


class Page(QScrollArea):
    """One settings page: cards of rows."""
    def __init__(self, title):
        super().__init__()
        self.setWidgetResizable(True)
        self.setFrameShape(QFrame.Shape.NoFrame)
        body = QWidget()
        body.setObjectName('settingsBody')
        self.column = QVBoxLayout(body)
        self.column.setContentsMargins(4, 4, 18, 24)
        self.column.setSpacing(16)
        heading = QLabel(title)
        heading.setObjectName('pageHeading')
        self.column.addWidget(heading)
        self.column.addStretch()
        self.setWidget(body)
        self.card_layout = None
        self.rows = 0

    def card(self, title=''):
        card = QFrame()
        card.setObjectName('card')
        layout = QVBoxLayout(card)
        layout.setContentsMargins(22, 14, 22, 14)
        layout.setSpacing(0)
        if title:
            label = QLabel(title.upper())
            label.setObjectName('cardTitle')
            layout.addWidget(label)
        self.column.insertWidget(self.column.count() - 1, card)
        self.card_layout = layout
        self.rows = 0
        return layout

    def row(self, title, control=None, help=None):
        """A row: title and help at the left, its control at the right. Tapping the text flips a switch."""
        row = QFrame()
        row.setObjectName('row')
        line = QHBoxLayout(row)
        line.setContentsMargins(0, 12, 0, 12)
        line.setSpacing(24)
        words = QVBoxLayout()
        words.setSpacing(3)
        name = QLabel(title)
        name.setObjectName('rowTitle')
        name.setWordWrap(True)
        words.addWidget(name)
        row.help_label = None
        if help is not None:
            text = QLabel(help)
            text.setObjectName('rowHelp')
            text.setWordWrap(True)
            words.addWidget(text)
            row.help_label = text
            if not help:
                text.hide()
        line.addLayout(words, 1)
        if control is not None:
            line.addWidget(control, 0, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
            if isinstance(control, Switch):
                name.mousePressEvent = lambda event: control.toggle()
                control.setAccessibleName(title)
        if self.rows:
            rule = QFrame()
            rule.setObjectName('rule')
            rule.setFixedHeight(1)
            self.card_layout.addWidget(rule)
        self.card_layout.addWidget(row)
        self.rows += 1
        return row


def buttons(*items):
    holder = QWidget()
    holder.setObjectName('buttons')
    line = QHBoxLayout(holder)
    line.setContentsMargins(0, 0, 0, 0)
    line.setSpacing(10)
    for item in items:
        line.addWidget(item)
    return holder


def build(window):
    """The settings page for ``window`` (a MainWindow); keeps the controls other code reaches as attributes."""
    session = window.session
    view, render = session.view, session.render
    outer = QWidget()
    outer.setObjectName('settings')
    column = QVBoxLayout(outer)
    column.setContentsMargins(30, 20, 30, 20)
    column.setSpacing(16)
    top = QHBoxLayout()
    heading = QLabel('Settings')
    heading.setObjectName('pageTitle')
    top.addWidget(heading)
    top.addStretch()
    done = window.button('DONE', window.toggle_settings)
    done.setObjectName('primary')
    top.addWidget(done)
    column.addLayout(top)
    body = QHBoxLayout()
    body.setSpacing(22)
    menu = QListWidget()
    menu.setObjectName('settingsMenu')
    menu.setAccessibleName('Settings sections')
    menu.setFixedWidth(240)
    menu.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
    stack = QStackedWidget()
    stack.setObjectName('settingsPages')
    menu.currentRowChanged.connect(stack.setCurrentIndex)
    body.addWidget(menu)
    body.addWidget(stack, 1)
    column.addLayout(body, 1)
    window.settings_menu = menu

    def page(name, title):
        p = Page(title)
        stack.addWidget(p)
        menu.addItem(name)
        return p

    def switch(key, default=True, on_change=None):
        control = Switch(bool(view.get(key, default)))
        control.toggled.connect(on_change or (lambda value, k=key: window.set_view(k, value)))
        return control

    def segmented(key, options, on_change=None):
        control = Segmented(options, view.get(key))
        control.changed.connect(on_change or (lambda value, k=key: window.set_view(k, value)))
        return control

    # DISPLAY ---------------------------------------------------------------------------------------
    p = page('DISPLAY', 'Display')
    p.card('Screen')
    window.screen_combo = combo(accessible='Target monitor')
    window.screen_combo.activated.connect(window.select_screen)
    p.row('Monitor', window.screen_combo, 'Wave fills this screen whenever it is connected.')
    window.refresh_screens()
    window.fullscreen_check = Switch(render.fill_display)
    window.fullscreen_check.toggled.connect(window.change_fullscreen)
    p.row('Full screen', window.fullscreen_check, 'Cover the whole monitor, taskbar included.')
    p.row('Keep Wave on top', switch('always_on_top', on_change=lambda v: (window.set_view('always_on_top', v),
                                                                            window.set_on_top(v))),
          'Other windows never cover the decks.')
    p.row('Booth dimmer', segmented('dim', [('100%', 1.0), ('80%', .8), ('60%', .6), ('40%', .4)],
                                    lambda v: window.set_view('dim', v)),
          'Darken the whole screen in a dark booth.')
    from . import touch_keyboard
    if touch_keyboard.available():
        p.card('Windows touch keyboard')
        try:
            current = touch_keyboard.mode()
        except OSError:
            current = touch_keyboard.NO_KEYBOARD
        keyboard = Segmented([('Never', touch_keyboard.NEVER), ('When no keyboard is attached', touch_keyboard.NO_KEYBOARD)],
                             current)
        keyboard.changed.connect(window.touch.choose)
        window.touch_segment = keyboard
        p.row('Show the touch keyboard', keyboard, "Windows' own keyboard, for typing in other apps. Wave has its own.")
        p.row('Turn it off while Wave runs', switch('touch_keyboard_off'),
              'Off when Wave starts, back as it was when Wave closes.')
    p.card('Decks')
    count = Segmented([('2 decks', 2), ('4 decks', 4)], render.deck_count)
    count.changed.connect(window.set_count)
    window.count_segment = count
    p.row('Layout', count, 'Two or four stacked waveforms.')
    window.order_combo = combo([('FLX10 · 3, 1, 2, 4', '3,1,2,4'), ('Numeric · 1, 2, 3, 4', '1,2,3,4'),
                                ('Paired · 1, 3, 2, 4', '1,3,2,4'), ('Reverse · 4, 3, 2, 1', '4,3,2,1')],
                               render.deck_order, 'Deck order')
    window.order_combo.setEditable(True)
    window.order_combo.currentIndexChanged.connect(lambda _: window.set_custom_order())
    window.order_combo.lineEdit().editingFinished.connect(window.set_custom_order)
    p.row('Deck order', window.order_combo, 'Top to bottom. Type your own, such as 2,1,4,3.')
    p.card('Text')
    scale = Segmented([('Standard', 1.0), ('Large', 1.25), ('Larger', 1.5), ('Largest', 2.0)], view.get('settings_scale', 1.0))
    scale.changed.connect(window.set_settings_scale)
    p.row('Settings text', scale, 'The size of everything on this page.')
    ui = Segmented([('Small', .85), ('Standard', 1.0), ('Large', 1.25), ('Larger', 1.5)], render.ui_scale)
    ui.changed.connect(lambda v: window.set_render('ui_scale', v))
    p.row('Deck text', ui, 'Cue flags, bar numbers and deck numbers on the waveforms.')

    # WAVEFORM --------------------------------------------------------------------------------------
    p = page('WAVEFORM', 'Waveform')
    p.card('View')
    window.zoom_combo = combo([(f'{b // 4} bars · {b} beats', b) for b in (4, 8, 16, 32, 64, 128, 256, 512)],
                              accessible='Waveform zoom')
    beats = view['zoom_beats']
    window.zoom_combo.setCurrentIndex(min(range(window.zoom_combo.count()),
                                          key=lambda i: abs(math.log2(window.zoom_combo.itemData(i) / beats))))
    window.zoom_combo.currentIndexChanged.connect(lambda _: window.set_view('zoom_beats', window.zoom_combo.currentData()))
    p.row('Zoom', window.zoom_combo, 'Also: pinch, scroll, or + and − on the keyboard.')
    window.position_combo = Segmented([('AZ · 32%', .32), ('Centre', .5), ('Left · 20%', .2)], view['playhead'])
    window.position_combo.changed.connect(lambda v: window.set_view('playhead', v))
    p.row('Playhead', window.position_combo, 'Where the playing point sits across the lane.')
    style = Segmented([('Rekordbox', 'Rekordbox'), ('Wave classic', 'Wave classic')], view.get('style', 'Rekordbox'))
    style.changed.connect(window.set_style)
    p.row('Colours', style, "Rekordbox's own three-band colours, or Wave's brighter set.")
    p.row('Band colours', buttons(*[window.button(b.upper(), lambda _=False, b=b: window.pick_color(b))
                                    for b in ('low', 'mid', 'high')]), 'Pick each band yourself.')
    p.card('Marks')
    for label, key, help in [('Beat and bar markers', 'grid', 'Red downbeats, white beats, outside the waveform.'),
                             ('Bar numbers', 'bar_numbers', 'Every fourth bar, under the lane.'),
                             ('Hot cues and memory cues', 'cues', 'Coloured flags, as Rekordbox has them.'),
                             ('Saved and live loops', 'loops', 'Brackets for saved loops, shading while one plays.')]:
        p.row(label, switch(key), help)
    p.card('Levels')
    for label, field in [('Low', 'low_gain'), ('Mid', 'mid_gain'), ('High', 'high_gain')]:
        spin = QDoubleSpinBox()
        spin.setRange(.1, 3)
        spin.setSingleStep(.1)
        spin.setSuffix(' ×')
        spin.setValue(getattr(render, field))
        spin.setMinimumWidth(160)
        spin.valueChanged.connect(lambda v, f=field: window.set_render(f, v, invalidate=True))
        p.row(f'{label} band level', spin)

    # DECK INFO -------------------------------------------------------------------------------------
    p = page('DECK INFO', 'Deck information')
    p.card()
    p.row('Information bar', switch('deck_info'), 'Track, time, BPM, key and the whole-track overview under the lanes.')
    p.row('Artwork', switch('artwork'), 'The sleeve beside each deck.')
    window.time_segment = segmented('time_mode', [('Remaining', 'remain'), ('Elapsed', 'elapsed')])
    p.row('Time', window.time_segment, 'Tap the time on a deck to flip it, too.')
    p.row('Bar size', segmented('info_size', [('Compact', .8), ('Standard', 1.0), ('Large', 1.25)]))
    p.row('End-of-track warning', segmented('end_warning', [('Off', 0), ('15 s', 15), ('30 s', 30), ('60 s', 60)]),
          'The time turns red and the overview flashes.')
    p.row('Battery', switch('battery'), 'Battery level in the top bar, and a warning when the charger comes out.')
    p.row('Master tempo in the top bar', switch('master_bpm'), "The tempo master's BPM, beside the clock.")

    # BROWSER ---------------------------------------------------------------------------------------
    p = page('BROWSER', 'Browser')
    p.card()
    window.browser_segment = segmented('browser', [("Wave's touch browser", 'wave'), ("Rekordbox's browser", 'rekordbox')])
    p.row('BROWSE opens', window.browser_segment,
          "Wave's touch browser: your collection or a USB stick, made for fingers. Rekordbox's: its own browser, over Wave.")
    window.controller_status = p.row('Controller', None, ' ').help_label
    p.row('Open a playlist with', segmented('tap_to_open', [('Double tap', 'double'), ('Single tap', 'single')]),
          'Double tap lets a finger scroll the list and pick without opening anything by accident.')
    p.row("The knob drives Wave's browser", switch('knob_loads_wave'),
          "Turning the browse knob moves Wave's highlight, and LOAD on the controller loads the track picked here.")

    # REKORDBOX -------------------------------------------------------------------------------------
    p = page('REKORDBOX', 'Rekordbox')
    p.card('Connection')
    window.connection_detail = p.row('Status', None, session.status or ' ').help_label
    if sys.platform in ('win32', 'darwin'):
        window.rekordbox_summary = p.row('Installed', None, ' ').help_label
        window.install_button = window.button('INSTALL REKORDBOX', window.install_rekordbox)
        window.install_button.setObjectName('primary')
        choose = window.button('USE A DOWNLOADED INSTALLER…', lambda: window.install_rekordbox(choose=True))
        page_button = window.button('DOWNLOAD PAGE', lambda: QDesktopServices.openUrl(QUrl('https://rekordbox.com/en/download/')))
        window.install_row = p.row('Install the supported Rekordbox', buttons(window.install_button, choose, page_button),
                                   'Wave gets AlphaTheta’s installer from rekordbox.com and runs it after checking its signature.')
        window.install_progress = QProgressBar()
        window.install_progress.setTextVisible(False)
        window.install_progress.setFixedHeight(6)
        window.install_progress.hide()
        p.card_layout.addWidget(window.install_progress)
        window.refresh_rekordbox_summary()
        p.card('Behind Wave')
        for label, key, help in [
                ('Start Rekordbox with Wave', 'start_rekordbox', 'Rekordbox starts, out of sight, when Wave does.'),
                ('Connect by itself', 'auto_connect', 'To a Rekordbox started without Wave. Windows connects live; macOS '
                                                     'restarts it only while no deck has a track.'),
                ('Start Rekordbox again if it closes', 'keep_rekordbox_running', 'Up to three times in ten minutes.'),
                ('Quit Rekordbox with Wave', 'quit_rekordbox', 'Never while a deck is playing.'),
                ('Keep Rekordbox hidden', 'hide_rekordbox', 'Minimised behind Wave.'),
                ("Close Rekordbox's notices and offers", 'close_popups', 'Questions are always left for you, '
                                                                         'brought over Wave.')]:
            p.row(label, switch(key), help)
        p.row('Rekordbox itself', buttons(window.button('SHOW REKORDBOX', window.show_rekordbox)),
              'For its preferences. Come back by tapping Wave.')
    p.card('Advanced')
    window.source_combo = combo([('Native · Rekordbox through Wave’s bridge', 'Native'), ('OSC · another tool', 'OSC'),
                                 ('Offline · analysis files', 'Offline')], view['source'], 'Playback connection')
    window.source_combo.currentIndexChanged.connect(lambda _: window.change_source(window.source_combo.currentData()))
    p.row('Playback connection', window.source_combo, 'OSC listens on localhost UDP 4460.')
    window.open_deck = combo([(f'Deck {i + 1}', i) for i in range(4)], 0, 'Deck for an analysis file')
    window.open_deck.setMinimumWidth(140)
    p.row('Open an analysis file', buttons(window.open_deck, window.button('OPEN…', window.open_analysis)),
          'Show a Rekordbox ANLZ file on a deck, for checking.')

    # SYSTEM ----------------------------------------------------------------------------------------
    p = page('SYSTEM', 'System')
    p.card('While Wave runs')
    p.row('Keep the screen awake', switch('keep_awake'), 'No screen saver or sleep during a set.')
    p.card('Updates')
    p.row('Check for updates', switch('check_updates'), 'When Wave starts and every few hours. Never installs during playback.')
    p.row('Now', buttons(window.button('CHECK FOR UPDATES', lambda: window.check_for_update(asked=True))))
    if sys.platform == 'darwin':
        p.card('Permissions')
        p.row('Accessibility', buttons(window.button('OPEN SETTINGS', lambda: QDesktopServices.openUrl(QUrl(
            'x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility')))),
            "Lets Wave read and close Rekordbox's popups.")
    p.card('Profiles')
    p.row('Settings profiles', buttons(window.button('IMPORT…', window.import_profile), window.button('EXPORT…', window.export_profile)),
          'Take your setup to another computer.')
    p.card('Problems')
    p.row('Log', buttons(window.button('OPEN LOG', window.open_log)), 'What Wave did, for a bug report.')
    p.row('Report a problem', buttons(window.button('OPEN GITHUB', lambda: QDesktopServices.openUrl(QUrl(REPO + '/issues/new/choose')))))
    p.row('Keys', None, 'F2 settings · F11 full screen · + / − zoom · Escape back')

    # ABOUT -----------------------------------------------------------------------------------------
    p = page('ABOUT', 'About')
    p.card()
    title = QLabel(f'Rekordbox Wave {__version__}')
    title.setObjectName('aboutTitle')
    p.card_layout.addWidget(title)
    about = QLabel('An XDJ-AZ style deck screen and touch browser for Rekordbox. Rekordbox stays the audio engine and '
                   'keeps your controller; Wave is the screen.')
    about.setObjectName('rowHelp')
    about.setWordWrap(True)
    p.card_layout.addWidget(about)
    p.row('Website', buttons(window.button('GITHUB', lambda: QDesktopServices.openUrl(QUrl(REPO)))))
    p.card('Credits and licences')
    credits = QLabel('Based on rkbx_wave by mrmilbe (MIT). Qt and PySide6 under the LGPL v3; pyrekordbox (MIT); '
                     'memory layouts from rkbx_link; waveform formats from Deep Symmetry. Rekordbox, XDJ-AZ and DDJ-FLX10 '
                     'are trademarks of AlphaTheta Corporation; Wave is not affiliated with or endorsed by AlphaTheta.')
    credits.setObjectName('rowHelp')
    credits.setWordWrap(True)
    p.card_layout.addWidget(credits)

    menu.setCurrentRow(0)
    return outer
