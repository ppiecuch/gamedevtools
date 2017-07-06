TEMPLATE = app
QT += widgets
CONFIG += debug

CONFIG(c++11): C11 = -c11
CONFIG(debug, debug|release): DBG = dbg
else: DBG = rel

DESTDIR = $$PWD/build-$$[QMAKE_SPEC]$$C11
SUBDIR = $${TEMPLATE}$${TARGET}.$${DBG}
OBJECTS_DIR = $$DESTDIR/$$SUBDIR/obj
MOC_DIR = $$DESTDIR/$$SUBDIR/ui
UI_DIR = $$DESTDIR/$$SUBDIR/ui
RCC_DIR = $$DESTDIR/$$SUBDIR/ui

INCLUDEPATH += ..
SOURCES += debug_font.cpp example.cpp
HEADERS += debug_font.h debug_font.txt  qopenglerrorcheck.h
RESOURCES += example.qrc

mac:QMAKE_CXXFLAGS += -Wno-narrowing
