import os
from pathlib import Path
import sys

from PyQt5.QtGui import QFont, QFontDatabase, QFontInfo


def prepare_fonts(app):
    if not QFontDatabase().families():
        if sys.platform != 'win32':
            raise RuntimeError('Qt test environment has no fonts; install a distribution font package')
        font = Path(os.environ['WINDIR'])/'Fonts/segoeui.ttf'
        identifier = QFontDatabase.addApplicationFont(str(font))
        families = QFontDatabase.applicationFontFamilies(identifier)
        if not families:
            raise RuntimeError('Could not load the existing Windows test font')
        app.setFont(QFont(families[0], 10))
    family = QFontInfo(app.font()).family()
    if not family:
        raise RuntimeError('Qt could not resolve a test font')
    return family
