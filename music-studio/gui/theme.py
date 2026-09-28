"""Dark DAW-style theme: colors and a global Qt stylesheet."""

BG = "#1b1b1f"
PANEL = "#232329"
PANEL_LIGHT = "#2b2b33"
BORDER = "#38383f"
TEXT = "#e8e8ec"
TEXT_DIM = "#9a9aa2"
ACCENT = "#5ee27a"       # FL-style green accent
ACCENT_DARK = "#2f8f47"
DANGER = "#ff5d5d"
WARN = "#f5c451"

TRACK_COLORS = [
    "#5ee27a", "#4fc3f7", "#ff8a65", "#ba68c8",
    "#ffd54f", "#4db6ac", "#f06292", "#9ccc65",
]

STYLESHEET = f"""
QMainWindow, QWidget {{
    background-color: {BG};
    color: {TEXT};
    font-family: "Segoe UI", "Inter", sans-serif;
    font-size: 12px;
}}

QScrollArea {{
    border: none;
    background-color: {BG};
}}

QFrame#trackCard {{
    background-color: {PANEL};
    border: 1px solid {BORDER};
    border-radius: 6px;
}}

QFrame#transportBar {{
    background-color: {PANEL};
    border: 1px solid {BORDER};
    border-radius: 6px;
}}

QLabel {{
    color: {TEXT};
}}

QLabel#trackName {{
    font-weight: 600;
    font-size: 13px;
}}

QLabel#dim {{
    color: {TEXT_DIM};
    font-size: 11px;
}}

QLineEdit {{
    background-color: {PANEL_LIGHT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 3px 6px;
    color: {TEXT};
    font-weight: 600;
}}

QPushButton {{
    background-color: {PANEL_LIGHT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 5px 12px;
    color: {TEXT};
}}
QPushButton:hover {{
    border-color: {ACCENT};
}}
QPushButton:pressed {{
    background-color: {ACCENT_DARK};
}}

QPushButton#primary {{
    background-color: {ACCENT_DARK};
    border: 1px solid {ACCENT};
    font-weight: 600;
    color: #0d1f10;
}}
QPushButton#primary:hover {{
    background-color: {ACCENT};
}}

QPushButton#recordBtn {{
    border: 1px solid {DANGER};
    font-weight: 600;
}}
QPushButton#recordBtn:checked {{
    background-color: {DANGER};
    color: #2b0000;
}}

QPushButton#toggleMute, QPushButton#toggleSolo {{
    min-width: 22px;
    max-width: 22px;
    min-height: 22px;
    max-height: 22px;
    padding: 0px;
    font-weight: 700;
    border-radius: 11px;
}}
QPushButton#toggleMute:checked {{
    background-color: {WARN};
    color: #2b1e00;
    border-color: {WARN};
}}
QPushButton#toggleSolo:checked {{
    background-color: {ACCENT};
    color: #0d1f10;
    border-color: {ACCENT};
}}

QPushButton#removeBtn {{
    min-width: 20px;
    max-width: 20px;
    min-height: 20px;
    max-height: 20px;
    padding: 0px;
    border-radius: 10px;
    color: {TEXT_DIM};
}}
QPushButton#removeBtn:hover {{
    background-color: {DANGER};
    color: white;
    border-color: {DANGER};
}}

QSlider::groove:horizontal {{
    height: 4px;
    background: {BORDER};
    border-radius: 2px;
}}
QSlider::handle:horizontal {{
    background: {ACCENT};
    border: none;
    width: 14px;
    height: 14px;
    margin: -6px 0;
    border-radius: 7px;
}}
QSlider::sub-page:horizontal {{
    background: {ACCENT_DARK};
    border-radius: 2px;
}}

QCheckBox {{
    spacing: 6px;
}}

QComboBox, QDoubleSpinBox {{
    background-color: {PANEL_LIGHT};
    border: 1px solid {BORDER};
    border-radius: 4px;
    padding: 3px 6px;
    color: {TEXT};
}}

QTabWidget::pane {{
    border: 1px solid {BORDER};
    border-radius: 6px;
    top: -1px;
}}
QTabBar::tab {{
    background: {PANEL};
    padding: 6px 14px;
    border: 1px solid {BORDER};
    border-bottom: none;
    border-top-left-radius: 5px;
    border-top-right-radius: 5px;
    margin-right: 2px;
}}
QTabBar::tab:selected {{
    background: {PANEL_LIGHT};
    border-color: {ACCENT};
    color: {ACCENT};
}}

QDialog {{
    background-color: {BG};
}}

QProgressDialog {{
    background-color: {PANEL};
}}

QScrollBar:vertical {{
    background: {BG};
    width: 10px;
}}
QScrollBar::handle:vertical {{
    background: {BORDER};
    border-radius: 5px;
    min-height: 20px;
}}
"""
