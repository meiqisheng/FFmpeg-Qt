#ifndef FFMPEGPLAYER_H
#define FFMPEGPLAYER_H

#include <QThread>
#include <QImage>
#include <QString>
#include <QMutex>           // ✅ 必需
#include <QWaitCondition>   // ✅ 必需
#include <QAudioOutput>
#include <QIODevice>
#include <QBuffer>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>       // 包含 av_image_get_buffer_size、av_image_fill_arrays
#include <libswscale/swscale.h>       // 包含 SwsContext、sws_getContext 等
}

class FFmpegPlayer : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegPlayer(QObject *parent = nullptr);
    ~FFmpegPlayer();

    void setFileName(const QString &filename);
    void stop();
    void pause();
    void resume();
    void setVolume(float volume);

signals:
    void frameReady(const QImage &image);
    void positionChanged(qint64 ms);
    void durationChanged(qint64 ms);
    void errorOccurred(const QString &message);  // 添加这行

public slots:
    void seekTo(qint64 ms); // 用于外部请求跳转

protected:
    void run() override;

private:
    QString m_fileName;
    bool m_running;
    bool m_paused = false;
    QMutex m_mutex;
    QWaitCondition m_pauseCond;
    qint64 m_duration = 0;
    qint64 m_seekPosition = -1; // 以毫秒为单位
    float m_volume = 1.0f;

    // 音频相关
    QAudioOutput *m_audioOutput = nullptr;
    QIODevice *m_audioDevice = nullptr;
    SwrContext *m_swrCtx = nullptr;

    void cleanup();
    void setupAudio(AVCodecContext *audioCodecCtx);
    void processAudioFrame(AVFrame *frame, AVCodecContext *audioCodecCtx);
};

#endif // FFMPEGPLAYER_H
