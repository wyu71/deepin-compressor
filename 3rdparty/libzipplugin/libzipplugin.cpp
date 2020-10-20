/*
* Copyright (C) 2019 ~ 2020 Uniontech Software Technology Co.,Ltd.
*
* Author:     gaoxiang <gaoxiang@uniontech.com>
*
* Maintainer: gaoxiang <gaoxiang@uniontech.com>
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "libzipplugin.h"
#include "common.h"
#include "queries.h"

#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QThread>
#include <qplatformdefs.h>
#include <QDirIterator>
#include <QTimer>
#include <QDataStream>

//#include <zlib.h>

LibzipPluginFactory::LibzipPluginFactory()
{
    registerPlugin<LibzipPlugin>();
}

LibzipPluginFactory::~LibzipPluginFactory()
{

}



LibzipPlugin::LibzipPlugin(QObject *parent, const QVariantList &args)
    : ReadWriteArchiveInterface(parent, args)
{
    qDebug() << "LibzipPlugin";
    m_ePlugintype = PT_Libzip;
}

LibzipPlugin::~LibzipPlugin()
{

}

PluginFinishType LibzipPlugin::list()
{
    qDebug() << "LibzipPlugin插件加载压缩包数据";
    m_stArchiveData.reset();

    // 处理加载流程
    int errcode = 0;
    zip_error_t err;

    zip_t *archive = zip_open(QFile::encodeName(m_strArchiveName).constData(), ZIP_RDONLY, &errcode);   // 打开压缩包文件
    zip_error_init_with_code(&err, errcode);

    //某些特殊文件，如.crx用zip打不开，需要替换minizip
    if (!archive) {
//        m_bAllEntry = true;
//        return minizip_list();
    }

    // 获取文件压缩包文件数目
    const auto nofEntries = zip_get_num_entries(archive, 0);

    // 循环构建数据
    for (zip_int64_t i = 0; i < nofEntries; i++) {
        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }

        handleArchiveData(archive, i);  // 构建数据
    }

    zip_close(archive);

    return PT_Nomral;
}

PluginFinishType LibzipPlugin::testArchive()
{
    return PT_Nomral;
}

PluginFinishType LibzipPlugin::extractFiles(const QVector<FileEntry> &files, const ExtractionOptions &options)
{
    qDebug() << "解压缩数据";

    int errcode = 0;
    zip_error_t err;

    // 打开压缩包
    zip_t *archive = zip_open(QFile::encodeName(m_strArchiveName).constData(), ZIP_RDONLY, &errcode);
    zip_error_init_with_code(&err, errcode);
    if (archive == nullptr) {
        // 特殊包操作
        // return minizip_extractFiles(files, options);
    }

    // 执行解压操作
    if (options.bAllExtract) {  // 全部解压
        qlonglong qExtractSize = 0;
        zip_int64_t nofEntries = zip_get_num_entries(archive, 0);

        for (zip_int64_t i = 0; i < nofEntries; ++i) {
            if (QThread::currentThread()->isInterruptionRequested()) {
                break;
            }

            // 解压单个文件
            if (!extractEntry(archive, i, options, qExtractSize)) {
                zip_close(archive);
                return PF_Error;
            }
        }
    } else { // 部分提取

    }

    zip_close(archive);
    return PT_Nomral;
}

PluginFinishType LibzipPlugin::addFiles(const QVector<FileEntry> &files, const CompressOptions &options)
{
    qDebug() << "添加压缩包数据";
    int errcode = 0;
    zip_error_t err;

    // Open archive.
    zip_t *archive = zip_open(QFile::encodeName(m_strArchiveName).constData(), ZIP_CREATE, &errcode); //filename()压缩包名
    zip_error_init_with_code(&err, errcode);
    if (!archive) {
        emit error(("Failed to open the archive: %1")); //ReadOnlyArchiveInterface::error
        return PF_Error;
    }

    m_filesize = 0;
    for (const FileEntry &e : files) {
        // 过滤上级目录（不对全路径进行压缩）
        QString strPath = QFileInfo(e.strFullPath).absolutePath() + QDir::separator();

        //取消按钮 结束
        if (QThread::currentThread()->isInterruptionRequested()) {
            break;
        }

        // If entry is a directory, traverse and add all its files and subfolders.
        if (QFileInfo(e.strFullPath).isDir()) {
            if (!writeEntry(archive, e.strFullPath, options, true, strPath)) {
                if (zip_close(archive)) {
                    emit error(("Failed to write archive."));
                    return PF_Error;
                }
                return PF_Error;
            }

            QDirIterator it(e.strFullPath,
                            QDir::AllEntries | QDir::Readable |
                            QDir::Hidden | QDir::NoDotAndDotDot,
                            QDirIterator::Subdirectories);

            while (!QThread::currentThread()->isInterruptionRequested() && it.hasNext()) {
                const QString path = it.next();

                if (QFileInfo(path).isDir()) {
                    if (!writeEntry(archive, path, options, true, strPath)) {
                        if (zip_close(archive)) {
                            emit error(("Failed to write archive."));
                            return PF_Error;
                        }
                        return PF_Error;
                    }
                } else {
                    if (!writeEntry(archive, path, options, false, strPath)) {
                        if (zip_close(archive)) {
                            emit error(("Failed to write archive."));
                            return PF_Error;
                        }
                        return PF_Error;
                    }
                }
                ++m_filesize;
            }
        } else {
            if (!writeEntry(archive, e.strFullPath, options, false, strPath)) {
                if (zip_close(archive)) {
                    emit error(("Failed to write archive."));
                    return PF_Error;
                }
                return PF_Error;
            }
        }
        ++m_filesize;
    }


    m_addarchive = archive;
    // TODO:Register the callback function to get progress feedback.
    zip_register_progress_callback_with_state(archive, 0.001, progressCallback, nullptr, this);
    zip_register_cancel_callback_with_state(archive, cancelCallback, nullptr, this);

    if (zip_close(archive)) {
        emit error(("Failed to write archive."));

        return PF_Error;
    }

    // We list the entire archive after adding files to ensure entry
    // properties are up-to-date.


    return PT_Nomral;
}

PluginFinishType LibzipPlugin::moveFiles(const QVector<FileEntry> &files, const CompressOptions &options)
{
    return PT_Nomral;
}

PluginFinishType LibzipPlugin::copyFiles(const QVector<FileEntry> &files, const CompressOptions &options)
{
    return PT_Nomral;
}

PluginFinishType LibzipPlugin::deleteFiles(const QVector<FileEntry> &files)
{
    return PT_Nomral;
}

PluginFinishType LibzipPlugin::addComment(const QString &comment)
{
    return PT_Nomral;
}


bool LibzipPlugin::writeEntry(zip_t *archive, const QString &entry, const CompressOptions &options, bool isDir, const QString &strRoot)
{
    Q_ASSERT(archive);

    QString str;
    if (!options.strDestination.isEmpty()) {
        str = QString(options.strDestination + entry.mid(strRoot.length()));
    } else {
        //移除前缀路径
        str = entry.mid(strRoot.length());
    }

    zip_int64_t index;
    if (isDir) {
        index = zip_dir_add(archive, str.toUtf8().constData(), ZIP_FL_ENC_GUESS);
        if (index == -1) {
            // If directory already exists in archive, we get an error.
            return true;
        }
    } else {
        // 获取源文件
        zip_source_t *src = zip_source_file(archive, QFile::encodeName(entry).constData(), 0, -1);
        if (!src) {
            emit error(("Failed to add entry: %1"));
            return false;
        }

        // 向压缩包中添加文件
        index = zip_file_add(archive, str.toUtf8().constData(), src, ZIP_FL_ENC_GUESS | ZIP_FL_OVERWRITE);
        if (index == -1) {
            zip_source_free(src);
            emit error(("Failed to add entry: %1"));
            return false;
        }
    }

    zip_uint64_t uindex = static_cast<zip_uint64_t>(index);
#ifndef Q_OS_WIN
    // 设置文件权限
    QT_STATBUF result;
    if (QT_STAT(QFile::encodeName(entry).constData(), &result) != 0) {
    } else {
        zip_uint32_t attributes = result.st_mode << 16;
        if (zip_file_set_external_attributes(archive, uindex, ZIP_FL_UNCHANGED, ZIP_OPSYS_UNIX, attributes) != 0) {
        }
    }
#endif

    // 设置压缩的加密算法
    if (options.bEncryption && !options.strEncryptionMethod.isEmpty()) { //ReadOnlyArchiveInterface::password()
        int ret = 0;
        if (options.strEncryptionMethod == QLatin1String("AES128")) {
            ret = zip_file_set_encryption(archive, uindex, ZIP_EM_AES_128, options.strPassword.toUtf8().constData());
        } else if (options.strEncryptionMethod == QLatin1String("AES192")) {
            ret = zip_file_set_encryption(archive, uindex, ZIP_EM_AES_192, options.strPassword.toUtf8().constData());
        } else if (options.strEncryptionMethod == QLatin1String("AES256")) {
            ret = zip_file_set_encryption(archive, uindex, ZIP_EM_AES_256, options.strPassword.toUtf8().constData());
        }
        if (ret != 0) {
            emit error(("Failed to set compression options for entry: %1"));
            return false;
        }
    }

    // 设置压缩算法
    zip_int32_t compMethod = ZIP_CM_DEFAULT;
    if (!options.strCompressionMethod.isEmpty()) {
        if (options.strCompressionMethod == QLatin1String("Deflate")) {
            compMethod = ZIP_CM_DEFLATE;
        } else if (options.strCompressionMethod == QLatin1String("BZip2")) {
            compMethod = ZIP_CM_BZIP2;
        } else if (options.strCompressionMethod == QLatin1String("Store")) {
            compMethod = ZIP_CM_STORE;
        }
    }

    // 设置压缩等级
    const int compLevel = (options.iCompressionLevel != -1) ? options.iCompressionLevel : 6;
    if (zip_set_file_compression(archive, uindex, compMethod, compLevel) != 0) {
        emit error(("Failed to set compression options for entry: %1"));
        return false;
    }

    return true;
}


void LibzipPlugin::progressCallback(zip_t *, double progress, void *that)
{
    //qDebug() << progress;
    static_cast<LibzipPlugin *>(that)->emitProgress(progress);      // 进度回调
}


int LibzipPlugin::cancelCallback(zip_t *, void *that)
{
    return 0;
    //return static_cast<LibzipPlugin *>(that)->cancelResult();       // 取消回调
}

bool LibzipPlugin::handleArchiveData(zip_t *archive, zip_int64_t index)
{
    if (archive == nullptr) {
        return false;
    }

    zip_stat_t statBuffer;
    if (zip_stat_index(archive, zip_uint64_t(index), ZIP_FL_ENC_RAW, &statBuffer)) {
        return false;
    }

    // 对文件名进行编码探测并转码
    QString name = m_common->trans2uft8(statBuffer.name);

    FileEntry entry;
    entry.iIndex = int(index);
    entry.strFullPath = name;
    statBuffer2FileEntry(statBuffer, entry);

    // 获取第一层数据
    if (!name.contains(QDir::separator()) || (name.count(QDir::separator()) == 1 && name.endsWith(QDir::separator()))) {
        m_stArchiveData.listRootEntry.push_back(entry);
    }

    // 存储总数据
    m_stArchiveData.mapFileEntry[name] = entry;

    return true;
}

void LibzipPlugin::statBuffer2FileEntry(const zip_stat_t &statBuffer, FileEntry &entry)
{
    // FileEntry stFileEntry;

    // 文件名
    if (statBuffer.valid & ZIP_STAT_NAME) {
        const QStringList pieces = entry.strFullPath.split(QLatin1Char('/'), QString::SkipEmptyParts);
        entry.strFileName = pieces.isEmpty() ? QString() : pieces.last();
    }

    // 是否为文件夹
    if (entry.strFullPath.endsWith(QDir::separator())) {
        entry.isDirectory = true;
    }

    // 文件真实大小（文件夹显示项）
    if (statBuffer.valid & ZIP_STAT_SIZE) {
        if (!entry.isDirectory) {
            entry.qSize = qlonglong(statBuffer.size);
            m_stArchiveData.qSize += statBuffer.size;
            m_stArchiveData.qComressSize += statBuffer.comp_size;
        } else {
            entry.qSize = 0;
        }
    }

    // 文件最后修改时间
    if (statBuffer.valid & ZIP_STAT_MTIME) {
        entry.uLastModifiedTime = uint(statBuffer.mtime);
    }

}

bool LibzipPlugin::extractEntry(zip_t *archive, zip_int64_t index, const ExtractionOptions &options, qlonglong &qExtractSize)
{
    zip_stat_t statBuffer;
    if (zip_stat_index(archive, zip_uint64_t(index), ZIP_FL_ENC_RAW, &statBuffer)) {
        return false;
    }

    QString strFileName = m_common->trans2uft8(statBuffer.name);    // 解压文件名（压缩包中）
    emit signalCurFileName(strFileName);        // 发送当前正在解压的文件名
    bool bIsDirectory = strFileName.endsWith(QDir::separator());    // 是否为文件夹

    // 解压完整文件名（含路径）
    QString strDestFileName = options.strTargetPath + QDir::separator() + strFileName;
    QFile file(strDestFileName);

    // 获取外部信息（权限）
    zip_uint8_t opsys;
    zip_uint32_t attributes;
    if (zip_file_get_external_attributes(archive, zip_uint64_t(index), ZIP_FL_UNCHANGED, &opsys, &attributes) == -1) {
        emit error(("Failed to read metadata for entry: %1"));
    }
    mode_t value = attributes >> 16;
    QFileDevice::Permissions per = getPermissions(value);

    if (bIsDirectory) {     // 文件夹
        QDir dir;
        dir.mkpath(strDestFileName);

        // 文件夹加可执行权限
        per = per | QFileDevice::ReadUser | QFileDevice::WriteUser | QFileDevice::ExeUser ;
    } else {        // 普通文件

        // 判断是否有同名文件
        if (m_bSkipAll) {
            return true;
        } else {
            if (!m_bOverwriteAll) {

//               OverwriteQuery query(QFileInfo(strDestFileName).fileName());
//               emit signalQuery(&query);
//               query.waitForResponse();

//                if (query.responseCancelled()) {
//                    emit signalCancel();
//                    return false;
//                } else if (query.responseSkip()) {
//                    return enum_extractEntryStatus::SUCCESS;
//                } else if (query.responseAutoSkip()) {
//                    m_skipAll = true;
//                    return enum_extractEntryStatus::SUCCESS;
//                } else if (query.responseRename()) {
//                    const QString newName(query.newFilename());
//                    destination = QFileInfo(destination).path() + QDir::separator() + QFileInfo(newName).fileName();
//                    renamedEntry = QFileInfo(entry).path() + QDir::separator() + QFileInfo(newName).fileName();
//                } else if (query.responseOverwriteAll()) {
//                    m_overwriteAll = true;
//                    break;
//                } else if (query.responseOverwrite()) {
//                    break;
//                }
            }
        }


        zip_file_t *zipFile = zip_fopen_index(archive, zip_uint64_t(index), 0);
        if (zipFile == nullptr) {
            // 错误处理
        }

        // 以只写的方式打开待解压的文件
        if (file.open(QIODevice::WriteOnly) == false) {
            return false;
        }

        // 写文件
        QDataStream out(&file);
        int kb = 1024;
        zip_int64_t sum = 0;
        char buf[kb];
        int writeSize = 0;
        while (sum != zip_int64_t(statBuffer.size)) {

            const auto readBytes = zip_fread(zipFile, buf, zip_uint64_t(kb));
            if (readBytes < 0) {

                file.close();
                zip_fclose(zipFile);
                return false;
            }

            if (out.writeRawData(buf, int(readBytes)) != readBytes) {
                file.close();
                zip_fclose(zipFile);
                return false;
            }

            sum += readBytes;
            writeSize += readBytes;

            // 计算进度并显示（右键快捷解压使用压缩包大小，计算比例）
            qExtractSize += readBytes;
            if (options.bRightExtract) {
                emit signalprogress((double(qExtractSize * options.qComressSize / options.qSize)) / options.qComressSize * 100);
            } else {
                emit signalprogress((double(qExtractSize)) / options.qSize * 100);
            }
        }

        file.close();
        zip_fclose(zipFile);
    }

    // 设置文件/文件夹权限
    file.setPermissions(per);

    return true;
}

void LibzipPlugin::emitProgress(double dPercentage)
{
    zip_uint64_t index = zip_uint64_t(m_filesize * dPercentage);

    if (m_addarchive) {
        emit signalCurFileName(m_common->trans2uft8(zip_get_name(m_addarchive, index, ZIP_FL_ENC_RAW)));
    }

    emit signalprogress(dPercentage * 100);
}
