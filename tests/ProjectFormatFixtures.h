#pragma once

#include <QCborArray>
#include <QCborMap>
#include <QJsonArray>
#include <QJsonObject>
#include <stdexcept>

namespace project_fixture {
inline constexpr char signature[] = "BENDSIM";

inline QCborMap compactRoot(const QByteArray &bytes) {
    const QByteArray header(signature, sizeof(signature));
    if (!bytes.startsWith(header))
        throw std::runtime_error("Missing project signature");
    QCborParserError error;
    const auto value = QCborValue::fromCbor(bytes.sliced(header.size()), &error);
    if (error.error != QCborError::NoError || error.offset != bytes.size() - header.size() ||
        !value.isMap() || value.toMap().value("version").toInteger() != 20)
        throw std::runtime_error("Invalid compact project fixture");
    return value.toMap();
}

inline QByteArray compactBytes(const QCborMap &root) {
    return QByteArray(signature, sizeof(signature)) + QCborValue(root).toCbor();
}

// Build genuine legacy JSON inputs from a fresh snapshot. Tests can then drop
// fields introduced after the legacy version they exercise.
inline QJsonObject legacyRoot(const QByteArray &bytes, int version = 18) {
    auto compact = compactRoot(bytes);
    auto source = compact.value("source").toMap();
    const auto step = source.value("data");
    if (!step.isByteArray())
        throw std::runtime_error("STEP snapshot is not a byte string");
    source.insert(QStringLiteral("data"), QString::fromLatin1(step.toByteArray().toBase64()));
    compact.insert(QStringLiteral("source"), source);
    auto root = compact.toJsonObject();
    const auto assignments = root.take("toolAssignments").toArray();
    auto bends = root.value("bends").toArray();
    for (qsizetype i = 0; i < bends.size(); ++i) {
        auto bend = bends[i].toObject();
        const auto reference = bend.value("catalogTools");
        if (!reference.isNull())
            bend.insert("catalogTools", assignments[reference.toInt(-1)]);
        bends[i] = bend;
    }
    root.insert("bends", bends);
    root.remove("drawing");
    root.insert("version", version);
    return root;
}
} // namespace project_fixture
