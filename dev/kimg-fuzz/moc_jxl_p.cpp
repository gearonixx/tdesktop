/****************************************************************************
** Meta object code from reading C++ file 'jxl_p.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../Telegram/ThirdParty/kimageformats/src/imageformats/jxl_p.h"
#include <QtCore/qmetatype.h>
#include <QtCore/qplugin.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'jxl_p.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.11.1. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN13QJpegXLPluginE_t {};
} // unnamed namespace

template <> constexpr inline auto QJpegXLPlugin::qt_create_metaobjectdata<qt_meta_tag_ZN13QJpegXLPluginE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "QJpegXLPlugin"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<QJpegXLPlugin, qt_meta_tag_ZN13QJpegXLPluginE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject QJpegXLPlugin::staticMetaObject = { {
    QMetaObject::SuperData::link<QImageIOPlugin::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13QJpegXLPluginE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13QJpegXLPluginE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN13QJpegXLPluginE_t>.metaTypes,
    nullptr
} };

void QJpegXLPlugin::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<QJpegXLPlugin *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *QJpegXLPlugin::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *QJpegXLPlugin::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN13QJpegXLPluginE_t>.strings))
        return static_cast<void*>(this);
    return QImageIOPlugin::qt_metacast(_clname);
}

int QJpegXLPlugin::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QImageIOPlugin::qt_metacall(_c, _id, _a);
    return _id;
}

#ifdef QT_MOC_EXPORT_PLUGIN_V2
static constexpr unsigned char qt_pluginMetaDataV2_QJpegXLPlugin[] = {
    0xbf, 
    // "IID"
    0x02,  0x78,  0x31,  'o',  'r',  'g',  '.',  'q', 
    't',  '-',  'p',  'r',  'o',  'j',  'e',  'c', 
    't',  '.',  'Q',  't',  '.',  'Q',  'I',  'm', 
    'a',  'g',  'e',  'I',  'O',  'H',  'a',  'n', 
    'd',  'l',  'e',  'r',  'F',  'a',  'c',  't', 
    'o',  'r',  'y',  'I',  'n',  't',  'e',  'r', 
    'f',  'a',  'c',  'e', 
    // "className"
    0x03,  0x6d,  'Q',  'J',  'p',  'e',  'g',  'X', 
    'L',  'P',  'l',  'u',  'g',  'i',  'n', 
    // "MetaData"
    0x04,  0xa2,  0x64,  'K',  'e',  'y',  's',  0x81, 
    0x63,  'j',  'x',  'l',  0x69,  'M',  'i',  'm', 
    'e',  'T',  'y',  'p',  'e',  's',  0x81,  0x69, 
    'i',  'm',  'a',  'g',  'e',  '/',  'j',  'x', 
    'l', 
    0xff, 
};
QT_MOC_EXPORT_PLUGIN_V2(QJpegXLPlugin, QJpegXLPlugin, qt_pluginMetaDataV2_QJpegXLPlugin)
#else
QT_PLUGIN_METADATA_SECTION
Q_CONSTINIT static constexpr unsigned char qt_pluginMetaData_QJpegXLPlugin[] = {
    'Q', 'T', 'M', 'E', 'T', 'A', 'D', 'A', 'T', 'A', ' ', '!',
    // metadata version, Qt version, architectural requirements
    0, QT_VERSION_MAJOR, QT_VERSION_MINOR, qPluginArchRequirements(),
    0xbf, 
    // "IID"
    0x02,  0x78,  0x31,  'o',  'r',  'g',  '.',  'q', 
    't',  '-',  'p',  'r',  'o',  'j',  'e',  'c', 
    't',  '.',  'Q',  't',  '.',  'Q',  'I',  'm', 
    'a',  'g',  'e',  'I',  'O',  'H',  'a',  'n', 
    'd',  'l',  'e',  'r',  'F',  'a',  'c',  't', 
    'o',  'r',  'y',  'I',  'n',  't',  'e',  'r', 
    'f',  'a',  'c',  'e', 
    // "className"
    0x03,  0x6d,  'Q',  'J',  'p',  'e',  'g',  'X', 
    'L',  'P',  'l',  'u',  'g',  'i',  'n', 
    // "MetaData"
    0x04,  0xa2,  0x64,  'K',  'e',  'y',  's',  0x81, 
    0x63,  'j',  'x',  'l',  0x69,  'M',  'i',  'm', 
    'e',  'T',  'y',  'p',  'e',  's',  0x81,  0x69, 
    'i',  'm',  'a',  'g',  'e',  '/',  'j',  'x', 
    'l', 
    0xff, 
};
QT_MOC_EXPORT_PLUGIN(QJpegXLPlugin, QJpegXLPlugin)
#endif  // QT_MOC_EXPORT_PLUGIN_V2

QT_WARNING_POP
