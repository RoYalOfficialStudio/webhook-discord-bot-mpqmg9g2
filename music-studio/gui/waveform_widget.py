from __future__ import annotations

import numpy as np
from PySide6.QtWidgets import QWidget
from PySide6.QtGui import QPainter, QColor, QPen
from PySide6.QtCore import Qt, Signal

from .theme import PANEL_LIGHT, BORDER


class WaveformWidget(QWidget):
    """Draws a min/max envelope of an audio buffer and lets the user drag out
    a selection to cut, like scrubbing a clip in FL Studio's playlist — a
    duration label alone doesn't show silence, clipping, or where the signal
    actually is, let alone let you remove part of it.
    """

    selectionChanged = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setMinimumHeight(56)
        self.setCursor(Qt.IBeamCursor)
        self._peaks: np.ndarray | None = None  # shape (n_columns, 2) -> (min, max)
        self._color = QColor("#5ee27a")
        self._audio_len = 0
        self._sel_start_frac: float | None = None
        self._sel_end_frac: float | None = None
        self._dragging = False

    def set_audio(self, audio: np.ndarray | None, color: str | None = None) -> None:
        if color:
            self._color = QColor(color)
        self.clear_selection()
        if audio is None or len(audio) == 0:
            self._peaks = None
            self._audio_len = 0
            self.update()
            return
        mono = np.mean(audio, axis=1) if audio.ndim > 1 else audio
        self._audio_len = len(mono)
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

    def clear_selection(self) -> None:
        self._sel_start_frac = None
        self._sel_end_frac = None
        self._dragging = False
        self.update()
        self.selectionChanged.emit()

    def get_selection_samples(self) -> tuple[int, int] | None:
        if not self._audio_len or self._sel_start_frac is None or self._sel_end_frac is None:
            return None
        lo = max(0.0, min(self._sel_start_frac, self._sel_end_frac))
        hi = min(1.0, max(self._sel_start_frac, self._sel_end_frac))
        start = int(lo * self._audio_len)
        end = int(hi * self._audio_len)
        return (start, end) if end - start > 0 else None

    def get_selection_seconds(self, sr: int) -> float | None:
        sel = self.get_selection_samples()
        if not sel or not sr:
            return None
        return (sel[1] - sel[0]) / sr

    def _x_to_frac(self, x: float) -> float:
        return min(1.0, max(0.0, x / max(1, self.width())))

    def mousePressEvent(self, event) -> None:
        if not self._audio_len:
            return
        self._dragging = True
        self._sel_start_frac = self._x_to_frac(event.position().x())
        self._sel_end_frac = self._sel_start_frac
        self.update()
        self.selectionChanged.emit()

    def mouseMoveEvent(self, event) -> None:
        if not self._dragging:
            return
        self._sel_end_frac = self._x_to_frac(event.position().x())
        self.update()
        self.selectionChanged.emit()

    def mouseReleaseEvent(self, event) -> None:
        self._dragging = False
        if self.get_selection_samples() is None:
            self.clear_selection()

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

        if self._sel_start_frac is not None and self._sel_end_frac is not None:
            x1 = int(min(self._sel_start_frac, self._sel_end_frac) * w)
            x2 = int(max(self._sel_start_frac, self._sel_end_frac) * w)
            if x2 > x1:
                painter.fillRect(x1, 0, x2 - x1, self.height(), QColor(255, 255, 255, 60))
                painter.setPen(QPen(QColor("#ffffff"), 1))
                painter.drawLine(x1, 0, x1, self.height())
                painter.drawLine(x2, 0, x2, self.height())
