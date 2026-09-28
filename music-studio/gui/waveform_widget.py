from __future__ import annotations

import numpy as np
from PySide6.QtWidgets import QWidget
from PySide6.QtGui import QPainter, QColor, QPen
from PySide6.QtCore import Qt

from .theme import PANEL_LIGHT, BORDER


class WaveformWidget(QWidget):
    """Draws a min/max envelope of an audio buffer — a quick "how much did I
    actually record" glance, since a duration label alone doesn't show silence,
    clipping, or where the signal actually is.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(48)
        self._peaks: np.ndarray | None = None  # shape (n_columns, 2) -> (min, max)
        self._color = QColor("#5ee27a")

    def set_audio(self, audio: np.ndarray, color: str | None = None) -> None:
        if color:
            self._color = QColor(color)
        if audio is None or len(audio) == 0:
            self._peaks = None
            self.update()
            return
        mono = np.mean(audio, axis=1) if audio.ndim > 1 else audio
        columns = max(1, min(400, len(mono)))
        chunk_size = max(1, len(mono) // columns)
        n = (len(mono) // chunk_size) * chunk_size
        if n == 0:
            self._peaks = None
            self.update()
            return
        reshaped = mono[:n].reshape(-1, chunk_size)
        self._peaks = np.stack([reshaped.min(axis=1), reshaped.max(axis=1)], axis=1)
        self.update()

    def paintEvent(self, event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        painter.fillRect(self.rect(), QColor(PANEL_LIGHT))
        painter.setPen(QPen(QColor(BORDER), 1))
        mid = self.height() / 2
        painter.drawLine(0, int(mid), self.width(), int(mid))

        if self._peaks is None or len(self._peaks) == 0:
            painter.setPen(QColor("#6a6a72"))
            painter.drawText(self.rect(), Qt.AlignCenter, "no audio yet")
            return

        painter.setPen(QPen(self._color, 1))
        n = len(self._peaks)
        w = self.width()
        for i, (lo, hi) in enumerate(self._peaks):
            x = int(i / n * w)
            y1 = mid - hi * mid
            y2 = mid - lo * mid
            painter.drawLine(x, int(y1), x, int(y2))
