/****************************************************************************
** Meta object code from reading C++ file 'heif_p.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.11.1)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../Telegram/ThirdParty/kimageformats/src/imageformats/heif_p.h"
#include <QtCore/qmetatype.h>
#include <QtCore/qplugin.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'heif_p.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN10HEIFPluginE_t {};
} // unnamed namespace

template <> constexpr inline auto HEIFPlugin::qt_create_metaobjectdata<qt_meta_tag_ZN10HEIFPluginE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "HEIFPlugin"
    };

    QtMocHelpers::UintData qt_methods {
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<HEIFPlugin, qt_meta_tag_ZN10HEIFPluginE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject HEIFPlugin::staticMetaObject = { {
    QMetaObject::SuperData::link<QImageIOPlugin::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10HEIFPluginE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10HEIFPluginE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN10HEIFPluginE_t>.metaTypes,
    nullptr
} };

void HEIFPlugin::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<HEIFPlugin *>(_o);
    (void)_t;
    (void)_c;
    (void)_id;
    (void)_a;
}

const QMetaObject *HEIFPlugin::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *HEIFPlugin::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN10HEIFPluginE_t>.strings))
        return static_cast<void*>(this);
    return QImageIOPlugin::qt_metacast(_clname);
}

int HEIFPlugin::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QImageIOPlugin::qt_metacall(_c, _id, _a);
    return _id;
}

#ifdef QT_MOC_EXPORT_PLUGIN_V2
static constexpr unsigned char qt_pluginMetaDataV2_HEIFPlugin[] = {
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
    0x03,  0x6a,  'H',  'E',  'I',  'F',  'P',  'l', 
    'u',  'g',  'i',  'n', 
    // "MetaData"
    0x04,  0xa2,  0x64,  'K',  'e',  'y',  's',  0x83, 
    0x64,  'h',  'e',  'i',  'f',  0x64,  'h',  'e', 
    'i',  'c',  0x64,  'h',  'e',  'j',  '2',  0x69, 
    'M',  'i',  'm',  'e',  'T',  'y',  'p',  'e', 
    's',  0x83,  0x6a,  'i',  'm',  'a',  'g',  'e', 
    '/',  'h',  'e',  'i',  'f',  0x6a,  'i',  'm', 
    'a',  'g',  'e',  '/',  'h',  'e',  'i',  'f', 
    0x6b,  'i',  'm',  'a',  'g',  'e',  '/',  'h', 
    'e',  'j',  '2',  'k', 
    0xff, 
};
QT_MOC_EXPORT_PLUGIN_V2(HEIFPlugin, HEIFPlugin, qt_pluginMetaDataV2_HEIFPlugin)
#else
QT_PLUGIN_METADATA_SECTION
Q_CONSTINIT static constexpr unsigned char qt_pluginMetaData_HEIFPlugin[] = {
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
    0x03,  0x6a,  'H',  'E',  'I',  'F',  'P',  'l', 
    'u',  'g',  'i',  'n', 
    // "MetaData"
    0x04,  0xa2,  0x64,  'K',  'e',  'y',  's',  0x83, 
    0x64,  'h',  'e',  'i',  'f',  0x64,  'h',  'e', 
    'i',  'c',  0x64,  'h',  'e',  'j',  '2',  0x69, 
    'M',  'i',  'm',  'e',  'T',  'y',  'p',  'e', 
    's',  0x83,  0x6a,  'i',  'm',  'a',  'g',  'e', 
    '/',  'h',  'e',  'i',  'f',  0x6a,  'i',  'm', 
    'a',  'g',  'e',  '/',  'h',  'e',  'i',  'f', 
    0x6b,  'i',  'm',  'a',  'g',  'e',  '/',  'h', 
    'e',  'j',  '2',  'k', 
    0xff, 
};
QT_MOC_EXPORT_PLUGIN(HEIFPlugin, HEIFPlugin)
#endif  // QT_MOC_EXPORT_PLUGIN_V2

QT_WARNING_POP
