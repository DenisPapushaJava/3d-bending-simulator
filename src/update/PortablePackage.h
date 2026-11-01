#pragma once
#include <QString>
#include <stop_token>
namespace bending::update {
QString stageRuntime(const QString &extracted, const QString &version, const QString &root,
                     std::stop_token stop);
QString copyVerifiedZip(const QString &source, const QString &destination, std::stop_token stop);
bool safePackagePath(const QString &path);
} // namespace bending::update
