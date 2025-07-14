/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_file_click_handler.h"

#include "core/click_handler_types.h"
#include "core/file_utilities.h"
#include "core/application.h"
#include "data/data_document.h"
#include "data/data_session.h"
#include "data/data_download_manager.h"
#include "data/data_photo.h"
#include "main/main_session.h"
#include <QTimer>
#include <QProcess>  // 引入 QProcess 类定义 [[2]][[6]]

FileClickHandler::FileClickHandler(FullMsgId context)
: _context(context) {
}

void FileClickHandler::setMessageId(FullMsgId context) {
	_context = context;
}

FullMsgId FileClickHandler::context() const {
	return _context;
}

not_null<DocumentData*> DocumentClickHandler::document() const {
	return _document;
}

DocumentWrappedClickHandler::DocumentWrappedClickHandler(
	ClickHandlerPtr wrapped,
	not_null<DocumentData*> document,
	FullMsgId context)
: DocumentClickHandler(document, context)
, _wrapped(wrapped) {
}

void DocumentWrappedClickHandler::onClickImpl() const {
	_wrapped->onClick({ Qt::LeftButton });
}

DocumentClickHandler::DocumentClickHandler(
	not_null<DocumentData*> document,
	FullMsgId context)
: FileClickHandler(context)
, _document(document) {
	setProperty(
		kDocumentLinkMediaProperty,
		reinterpret_cast<qulonglong>(_document.get()));
}

QString DocumentClickHandler::tooltip() const {
	return property(kDocumentFilenameTooltipProperty).value<QString>();
}

DocumentOpenClickHandler::DocumentOpenClickHandler(
	not_null<DocumentData*> document,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: DocumentClickHandler(document, context)
, _handler(std::move(callback)) {
	Expects(_handler != nullptr);
}

void DocumentOpenClickHandler::onClickImpl() const {
	_handler(context());
}

void checkFileSize(const QString& savename,const int &size, int maxAttempts = 300, int attempt = 0) {
	if (attempt >= maxAttempts) {
		qDebug() << "检查超时，停止监控";
		return;
	}

	// 检查文件大小
	QFileInfo fileInfo(savename);
	if (fileInfo.exists()  && (fileInfo.size() == size) )
	{
		qDebug() << fileInfo.size();
		File::Launch(savename);
		return;  // 文件符合条件，停止检查
	}

	QTimer::singleShot(1000, [savename,size, maxAttempts, attempt]() {
		checkFileSize(savename,size, maxAttempts, attempt + 1);
		});
}

void DocumentSaveClickHandler::Save(
		Data::FileOrigin origin,
		not_null<DocumentData*> data,
		Mode mode,
		Fn<void()> started) {
	if (data->isNull()) {
		return;
	}

	auto savename = QString();
	if (mode == Mode::ToCacheOrFile && data->saveToCache()) {
		data->save(origin, savename);
		return;
	}
	InvokeQueued(qApp, crl::guard(&data->session(), [=] {
		// If we call file dialog synchronously, it will stop
		// background thread timers from working which would
		// stop audio playback in voice chats / live streams.
		if (mode != Mode::ToNewFile && data->saveFromData()) {
			if (started) {
				started();
			}
			return;
		}
		const auto filepath = data->filepath(true);
		const auto fileinfo = QFileInfo();
		const auto filedir = filepath.isEmpty()
			? QDir()
			: fileinfo.dir();
		const auto filename = filepath.isEmpty()
			? QString()
			: fileinfo.fileName();
		const auto savename = DocumentFileNameForSave(
			data,
			(mode == Mode::ToNewFile),
			filename,
			filedir);
		if (!savename.isEmpty()) {
			data->save(origin, savename);
			if (started) {
				started();
			}

			// checkFileSize(savename, data->size);
		}
	}));
}

//点击保存
void DocumentSaveClickHandler::SaveAndTrack(
		FullMsgId itemId,
		not_null<DocumentData*> document,
		Mode mode,
		Fn<void()> started) {

			{
				const auto filepath = document->filepath(true);
				const auto fileinfo = QFileInfo();
				const auto filedir = filepath.isEmpty()
					? QDir()
					: fileinfo.dir();
				const auto filename = filepath.isEmpty()
					? QString()
					: fileinfo.fileName();

				//文件路径
				const auto savename = DocumentFileNameForSave(
					document,
					(mode == Mode::ToNewFile),
					filename,
					filedir);

				// 分离文件路径的目录、文件名和扩展名
				QFileInfo fileInfo(savename);
				QString baseName = fileInfo.baseName();  // 文件名（不含扩展名）
				QString suffix = fileInfo.suffix();      // 扩展名
				QString path = fileInfo.path();          // 文件路径

				// 使用正则表达式移除文件名末尾的 (数字) 部分
				QRegularExpression regex("\\s\\(\\d+\\)$");
				QString cleanedBaseName = baseName.replace(regex, "");

				qDebug() << document->id;

				qDebug() << "清理后的文件名:" << cleanedBaseName;

				// 拼接清理后的文件名和扩展名
				const QString nameBase = cleanedBaseName;

				const auto fileSize = document->size;

				// 定义一个 QStringList，相当于 JavaScript 中的数组
				QStringList pathArr;
				pathArr << path << "Z:\\tgfiles\\account\\a402dae4-8dce-4e32-b8c8-9994d154bb2d\\videos"; // 使用 << 操作符添加元素

				// 使用基于范围的 for 循环遍历 QStringList
				for (const QString& path : pathArr) {

					for (size_t i = 0; i < 5; i++)
					{
						QString fileName;
						if (i == 0)
						{
							fileName = path + "\\" + nameBase + "." + suffix;
						}
						else
						{
							fileName = path + "\\" + nameBase + u" (%1)."_q.arg(i + 1) + suffix;
						}

						qDebug() << fileName;

						QFileInfo fi(fileName);
						if (fi.exists() && fi.size() == fileSize)
						{
							File::Launch(fileName);
							return;
						}
					}
				}
			}


	Save(itemId ? itemId : Data::FileOrigin(), document, mode, [=] {
		if (document->loading() && !document->loadingFilePath().isEmpty()) {
			if (const auto item = document->owner().message(itemId)) {
				Core::App().downloadManager().addLoading({
					.item = item,
					.document = document,
				});
			}
		}
		if (started) {
			started();
		}
	});
}

void DocumentSaveClickHandler::onClickImpl() const {
	SaveAndTrack(context(), document());
}

DocumentCancelClickHandler::DocumentCancelClickHandler(
	not_null<DocumentData*> document,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: DocumentClickHandler(document, context)
, _handler(std::move(callback)) {
}

void DocumentCancelClickHandler::onClickImpl() const {
	const auto data = document();
	if (data->isNull()) {
		return;
	} else if (data->uploading() && _handler) {
		_handler(context());
	} else {
		data->cancel();
	}
}

void DocumentOpenWithClickHandler::Open(
		Data::FileOrigin origin,
		not_null<DocumentData*> data) {
	if (data->isNull()) {
		return;
	}

	data->saveFromDataSilent();
	const auto path = data->filepath(true);
	if (!path.isEmpty()) {
		File::OpenWith(path);
	} else {
		DocumentSaveClickHandler::Save(
			origin,
			data,
			DocumentSaveClickHandler::Mode::ToFile);
	}
}

void DocumentOpenWithClickHandler::onClickImpl() const {
	Open(context(), document());
}

PhotoClickHandler::PhotoClickHandler(
	not_null<PhotoData*> photo,
	FullMsgId context,
	PeerData *peer)
: FileClickHandler(context)
, _photo(photo)
, _peer(peer) {
	setProperty(
		kPhotoLinkMediaProperty,
		reinterpret_cast<qulonglong>(_photo.get()));
}

not_null<PhotoData*> PhotoClickHandler::photo() const {
	return _photo;
}

PeerData *PhotoClickHandler::peer() const {
	return _peer;
}

PhotoOpenClickHandler::PhotoOpenClickHandler(
	not_null<PhotoData*> photo,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: PhotoClickHandler(photo, context)
, _handler(std::move(callback)) {
	Expects(_handler != nullptr);
}

void PhotoOpenClickHandler::onClickImpl() const {
	_handler(context());
}

void PhotoSaveClickHandler::onClickImpl() const {
	const auto data = photo();
	if (data->isNull()) {
		return;
	} else {
		data->clearFailed(Data::PhotoSize::Large);
		data->load(context());
	}
}

PhotoCancelClickHandler::PhotoCancelClickHandler(
	not_null<PhotoData*> photo,
	Fn<void(FullMsgId)> &&callback,
	FullMsgId context)
: PhotoClickHandler(photo, context)
, _handler(std::move(callback)) {
}

void PhotoCancelClickHandler::onClickImpl() const {
	const auto data = photo();
	if (data->isNull()) {
		return;
	} else if (data->uploading() && _handler) {
		_handler(context());
	} else {
		data->cancel();
	}
}
