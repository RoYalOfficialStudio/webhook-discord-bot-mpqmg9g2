"""Entry point for the offline music studio desktop app."""
import sys

from PySide6.QtWidgets import QApplication

from gui.main_window import MainWindow
from gui.theme import STYLESHEET


def main() -> None:
    app = QApplication(sys.argv)
    app.setStyleSheet(STYLESHEET)
    window = MainWindow()
    window.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
