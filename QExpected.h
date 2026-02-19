#pragma once
#include <QVariant>
#include <QString>
#include <QDebug>

template<typename T>
class QExpected {
public:
    // Constructors
    static QExpected ok(const T& value) {
        QExpected result;
        result.m_isOk = true;
        result.m_value = QVariant::fromValue(value);
        return result;
    }
    
    static QExpected err(const QString& error) {
        QExpected result;
        result.m_isOk = false;
        result.m_error = error;
        return result;
    }
    
    // Accessors
    bool isOk() const { return m_isOk; }
    bool isErr() const { return !m_isOk; }
    
    T value() const {
        if (!m_isOk) {
            qWarning() << "Accessing value on error state:" << m_error;
            return T{}; // Return default-constructed T on error
        }
        if (!m_value.canConvert<T>()) {
            qWarning() << "Failed to convert QVariant payload to expected type";
            return T{};
        }
        return m_value.value<T>();
    }
    
    QString error() const { return m_error; }
    
    // Conversion to/from QVariant
    QVariant toVariant() const {
        QVariantMap map;
        map["isOk"] = m_isOk;
        if (m_isOk) {
            map["value"] = m_value;
        } else {
            map["error"] = m_error;
        }
        return map;
    }
    
    // Implicit conversion to QVariant
    operator QVariant() const {
        return toVariant();
    }
    
    // Explicit conversion from QVariant
    explicit QExpected(const QVariant& v) {
        *this = fromVariant(v);
    }
    
    static QExpected fromVariant(const QVariant& v) {
        if (!v.canConvert<QVariantMap>()) {
            return QExpected::err(QStringLiteral("Invalid serialized QExpected: top-level value is not a QVariantMap"));
        }

        const QVariantMap map = v.toMap();
        const auto hasValueIt = map.constFind(QStringLiteral("isOk"));
        if (hasValueIt == map.constEnd() || hasValueIt->metaType().id() != QMetaType::Bool) {
            return QExpected::err(QStringLiteral("Invalid serialized QExpected: missing or non-boolean 'hasValue'"));
        }

        QExpected result;
        result.m_isOk = hasValueIt->toBool();
        if (result.m_isOk) {
            const auto valueIt = map.constFind(QStringLiteral("value"));
            if (valueIt == map.constEnd()) {
                return QExpected::err(QStringLiteral("Invalid serialized QExpected: missing 'value' payload"));
            }
            if (!valueIt->canConvert<T>()) {
                return QExpected::err(QStringLiteral("Invalid serialized QExpected: 'value' payload type mismatch"));
            }
            result.m_value = *valueIt;
            result.m_error.clear();
        } else {
            const auto errorIt = map.constFind(QStringLiteral("error"));
            if (errorIt == map.constEnd()) {
                return QExpected::err(QStringLiteral("Invalid serialized QExpected: missing 'error' message"));
            }
            if (!errorIt->canConvert<QString>()) {
                return QExpected::err(QStringLiteral("Invalid serialized QExpected: non-string 'error' message"));
            }
            result.m_error = errorIt->toString();
            result.m_value = QVariant{};
        }
        return result;
    }

private:
    // Default constructor for internal use by factory methods
    QExpected() = default;
    
    bool m_isOk{false};
    QVariant m_value;
    QString m_error{QStringLiteral("Uninitialized QExpected")};
};

template<>
class QExpected<void> {
public:
    // Constructors
    static QExpected ok() {
        QExpected result;
        result.m_isOk = true;
        result.m_error.clear();
        return result;
    }

    static QExpected err(const QString& error) {
        QExpected result;
        result.m_isOk = false;
        result.m_error = error;
        return result;
    }

    // Accessors
    bool isOk() const { return m_isOk; }
    bool isErr() const { return !m_isOk; }

    void value() const {
        if (!m_isOk) {
            qWarning() << "Accessing value on error state:" << m_error;
        }
    }

    QString error() const { return m_error; }

    // Conversion to/from QVariant
    QVariant toVariant() const {
        QVariantMap map;
        map["isOk"] = m_isOk;
        if (!m_isOk) {
            map["error"] = m_error;
        }
        return map;
    }
    
    // Implicit conversion to QVariant
    operator QVariant() const {
        return toVariant();
    }
    
    // Explicit conversion from QVariant
    explicit QExpected(const QVariant& v) {
        *this = fromVariant(v);
    }

    static QExpected fromVariant(const QVariant& v) {
        if (!v.canConvert<QVariantMap>()) {
            return QExpected::err(QStringLiteral("Invalid serialized QExpected: top-level value is not a QVariantMap"));
        }

        const QVariantMap map = v.toMap();
        const auto hasValueIt = map.constFind(QStringLiteral("isOk"));
        if (hasValueIt == map.constEnd() || hasValueIt->metaType().id() != QMetaType::Bool) {
            return QExpected::err(QStringLiteral("Invalid serialized QExpected: missing or non-boolean 'hasValue'"));
        }

        QExpected result;
        result.m_isOk = hasValueIt->toBool();
        if (result.m_isOk) {
            result.m_error.clear();
        } else {
            const auto errorIt = map.constFind(QStringLiteral("error"));
            if (errorIt == map.constEnd()) {
                return QExpected::err(QStringLiteral("Invalid serialized QExpected: missing 'error' message"));
            }
            if (!errorIt->canConvert<QString>()) {
                return QExpected::err(QStringLiteral("Invalid serialized QExpected: non-string 'error' message"));
            }
            result.m_error = errorIt->toString();
        }
        return result;
    }

private:
    // Default constructor for internal use by factory methods
    QExpected() = default;
    
    bool m_isOk{false};
    QString m_error{QStringLiteral("Uninitialized QExpected")};
};

// Register QExpected types with Qt's meta-type system
Q_DECLARE_METATYPE(QExpected<QString>)
Q_DECLARE_METATYPE(QExpected<void>)