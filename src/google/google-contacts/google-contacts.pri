CONFIG += link_pkgconfig
PKGCONFIG += Qt5Contacts qtcontacts-sqlite-qt5-extensions contactcache-qt5
QT += gui

SOURCES += \
    $$PWD/googletwowaycontactsyncadaptor.cpp \
    $$PWD/googlecontactstream.cpp \
    $$PWD/googlecontactatom.cpp \
    $$PWD/googlecontactimagedownloader.cpp

HEADERS += \
    $$PWD/googletwowaycontactsyncadaptor.h \
    $$PWD/googlecontactstream.h \
    $$PWD/googlecontactatom.h \
    $$PWD/googlecontactimagedownloader.h

INCLUDEPATH += $$PWD

