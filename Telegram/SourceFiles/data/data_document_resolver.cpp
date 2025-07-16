/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "data/data_document_resolver.h"

#include <QDesktopServices>
#include <QProcess>

#include "base/options.h"
#include "base/platform/base_platform_info.h"
#include "boxes/abstract_box.h" // Ui::show().
#include "chat_helpers/ttl_media_layer_widget.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "core/mime_type.h"
#include "data/data_document.h"
#include "data/data_document_media.h"
#include "data/data_file_click_handler.h"
#include "data/data_session.h"
#include "history/history.h"
#include "history/history_item.h"
#include "history/view/media/history_view_gif.h"
#include "lang/lang_keys.h"
#include "media/player/media_player_instance.h"
#include "platform/platform_file_utilities.h"
#include "ui/boxes/confirm_box.h"
#include "ui/chat/chat_theme.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/checkbox.h"
#include "ui/wrap/slide_wrap.h"
#include "window/window_session_controller.h"
#include "styles/style_layers.h"

#include <QtCore/QBuffer>
#include <QtCore/QMimeType>
#include <QtCore/QMimeDatabase>

#include "data_channel.h"

namespace Data
{
    namespace
    {
        base::options::toggle OptionExternalVideoPlayer({
            .id = kOptionExternalVideoPlayer,
            .name = "External video player",
            .description = "Use system video player instead of the internal one. "
            "This disabes video playback in messages.",
        });

        void ConfirmDontWarnBox(
            not_null<Ui::GenericBox*> box,
            rpl::producer<TextWithEntities>&& text,
            rpl::producer<QString>&& check,
            rpl::producer<QString>&& confirm,
            Fn<void(bool)> callback)
        {
            auto checkbox = object_ptr<Ui::Checkbox>(
                box.get(),
                std::move(check),
                false,
                st::defaultBoxCheckbox);
            const auto weak = Ui::MakeWeak(checkbox.data());
            auto confirmed = crl::guard(weak, [=, callback = std::move(callback)]
            {
                const auto checked = weak->checked();
                box->closeBox();
                callback(checked);
            });
            Ui::ConfirmBox(box, {
                               .text = std::move(text),
                               .confirmed = std::move(confirmed),
                               .confirmText = std::move(confirm),
                           });
            auto padding = st::boxPadding;
            padding.setTop(padding.bottom());
            box->addRow(std::move(checkbox), std::move(padding));
            box->addRow(object_ptr<Ui::SlideWrap<Ui::FlatLabel>>(
                box,
                object_ptr<Ui::FlatLabel>(
                    box,
                    tr::lng_launch_dont_ask_settings(),
                    st::boxLabel)
            ))->toggleOn(weak->checkedValue());
        }

        void LaunchWithWarning(
            // not_null<Window::Controller*> controller,
            const QString& name,
            HistoryItem* item)
        {
            const auto nameType = Core::DetectNameType(name);
            const auto isIpReveal = (nameType != Core::NameType::Executable)
                && Core::IsIpRevealingPath(name);
            const auto extension = Core::FileExtension(name).toLower();

            auto& app = Core::App();
            auto& settings = app.settings();
            const auto warn = [&]
            {
                if (item && item->history()->peer->isVerified())
                {
                    return false;
                }
                return (isIpReveal && settings.ipRevealWarning())
                    || ((nameType == Core::NameType::Executable
                            || nameType == Core::NameType::Unknown)
                        && !settings.noWarningExtensions().contains(extension));
            }();
            if (extension.isEmpty())
            {
                // If you launch a file without extension, like "test", in case
                // there is an executable file with the same name in this folder,
                // like "test.bat", the executable file will be launched.
                //
                // Now we always force an Open With dialog box for such files.
                //
                // Let's force it for all platforms for files without extension.
                crl::on_main([=]
                {
                    Platform::File::UnsafeShowOpenWith(name);
                });
                return;
            }
            else if (!warn)
            {
                File::Launch(name);
                return;
            }
            const auto callback = [=, &app, &settings](bool checked)
            {
                if (checked)
                {
                    if (isIpReveal)
                    {
                        settings.setIpRevealWarning(false);
                    }
                    else
                    {
                        auto copy = settings.noWarningExtensions();
                        copy.emplace(extension);
                        settings.setNoWarningExtensions(std::move(copy));
                    }
                    app.saveSettingsDelayed();
                }
                File::Launch(name);
            };
            auto text = isIpReveal
                            ? tr::lng_launch_svg_warning(Ui::Text::WithEntities)
                            : ((nameType == Core::NameType::Executable)
                                   ? tr::lng_launch_exe_warning
                                   : tr::lng_launch_other_warning)(
                                lt_extension,
                                rpl::single(Ui::Text::Bold('.' + extension)),
                                Ui::Text::WithEntities);
            auto check = (isIpReveal
                              ? tr::lng_launch_exe_dont_ask
                              : tr::lng_launch_dont_ask)();
            auto confirm = ((nameType == Core::NameType::Executable)
                                ? tr::lng_launch_exe_sure
                                : tr::lng_launch_other_sure)();
            Ui::show(Box(
                ConfirmDontWarnBox,
                std::move(text),
                std::move(check),
                std::move(confirm),
                callback));
        }
    } // namespace

    const char kOptionExternalVideoPlayer[] = "external-video-player";

    base::binary_guard ReadBackgroundImageAsync(
        not_null<Data::DocumentMedia*> media,
        FnMut<QImage(QImage)> postprocess,
        FnMut<void(QImage&&)> done)
    {
        auto result = base::binary_guard();
        const auto gzipSvg = media->owner()->isPatternWallPaperSVG();
        crl::async([
                gzipSvg,
                bytes = media->bytes(),
                path = media->owner()->filepath(),
                postprocess = std::move(postprocess),
                guard = result.make_guard(),
                callback = std::move(done)
            ]() mutable
            {
                auto image = Ui::ReadBackgroundImage(path, bytes, gzipSvg);
                if (postprocess)
                {
                    image = postprocess(std::move(image));
                }
                crl::on_main(std::move(guard), [
                                 image = std::move(image),
                                 callback = std::move(callback)
                             ]() mutable
                             {
                                 callback(std::move(image));
                             });
            });
        return result;
    }

    /**
    * @brief 检查应用程序根目录下的 url.txt 文件并读取其内容。
    * @return 如果文件存在且读取成功，返回文件内容的 QString；否则返回一个空 QString。
    */
    QString GetBaseUrl()
    {
        // 1. 构建文件的完整路径
        // QCoreApplication::applicationDirPath() 获取可执行文件所在的目录
        QString filePath = QCoreApplication::applicationDirPath() + "/url.txt";

        // 2. 创建 QFile 对象
        QFile file(filePath);

        // 3. 检查文件是否存在，并且是否能以只读、文本模式成功打开
        if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            // 4. 创建 QTextStream 以便方便地读取文本
            QTextStream in(&file);

            // 建议设置编码，以防URL或文件中包含非ASCII字符导致乱码
            in.setCodec("UTF-8");

            // 5. 读取文件的全部内容
            QString content = in.readAll();

            // QFile 对象在析构时会自动关闭文件，但显式调用 close() 是个好习惯
            file.close();

            // 6. 返回读取到的内容
            return content;
        }

        // 7. 如果文件不存在或打开失败，返回一个默认构造的空 QString
        return QString();
    }

    void ResolveDocument(
        Window::SessionController* controller,
        not_null<DocumentData*> document,
        HistoryItem* item,
        MsgId topicRootId)
    {
        if (document->isNull())
        {
            return;
        }
        const auto msgId = item ? item->fullId() : FullMsgId();

        qDebug() << msgId.peer.value;
        qDebug() << msgId.msg.bare;


        const auto showDocument = [&]
        {
            if ((true || OptionExternalVideoPlayer.value()) //在文件下载完成后,可以直接使用外部播放器打开
                && document->isVideoFile()
                && !document->filepath().isEmpty())
            {
                File::Launch(document->location(false).fname);
            }
            else if (controller)
            {
                controller->openDocument(
                    document,
                    true,
                    {msgId, topicRootId});
            }
        };

        const auto media = document->createMediaView();
        const auto openImageInApp = [&]
        {
            if (document->size >= Images::kReadBytesLimit)
            {
                return false;
            }
            const auto& location = document->location(true);
            const auto mime = u"image/"_q;
            if (!location.isEmpty() && location.accessEnable())
            {
                const auto guard = gsl::finally([&]
                {
                    location.accessDisable();
                });
                const auto path = location.name();
                if (Core::MimeTypeForFile(QFileInfo(path)).name().startsWith(mime)
                    && QImageReader(path).canRead())
                {
                    showDocument();
                    return true;
                }
            }
            else if (document->mimeString().startsWith(mime)
                && !media->bytes().isEmpty())
            {
                auto bytes = media->bytes();
                auto buffer = QBuffer(&bytes);
                if (QImageReader(&buffer).canRead())
                {
                    showDocument();
                    return true;
                }
            }
            return false;
        };
        const auto& location = document->location(true);
        if (document->isTheme() && media->loaded(true))
        {
            showDocument();
            location.accessDisable();
        }
        else if (media->canBePlayed(item))
        {
            if (document->isAudioFile()
                || document->isVoiceMessage()
                || document->isVideoMessage())
            {
                ::Media::Player::instance()->playPause({document, msgId});
                if (controller
                    && item
                    && item->media()
                    && item->media()->ttlSeconds())
                {
                    ChatHelpers::ShowTTLMediaLayerWidget(controller, item);
                }
            }
            else
            {
                //if (document->size > (150 * 1024 * 10240))
                showDocument();

                // QTimer::singleShot(1000, [item,document]() {
                // 		DocumentSaveClickHandler::Save(
                // item ? item->fullId() : Data::FileOrigin(),
                // document);
                // });
            }
        }
        else
        {
            document->saveFromDataSilent();
            if (!openImageInApp())
            {
                if (!document->filepath(true).isEmpty())
                {
                    LaunchWithWarning(location.name(), item);
                }
                else if (document->status == FileReady
                    || document->status == FileDownloadFailed)
                {
                    DocumentSaveClickHandler::Save(
                        item ? item->fullId() : Data::FileOrigin(),
                        document);
                }
            }
        }
    }
} // namespace Data
