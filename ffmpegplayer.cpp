#include "ffmpegplayer.h"
#include <QDebug>
#include <QThread>

FFmpegPlayer::FFmpegPlayer(QObject *parent)
    : QThread(parent), m_running(false)
{
    av_log_set_level(AV_LOG_ERROR);  // 可选：屏蔽FFmpeg日志
    av_register_all();               // 对于 FFmpeg < 4.0 需要；>4.0 可省略
}

FFmpegPlayer::~FFmpegPlayer()
{
    stop();
    wait();
}

void FFmpegPlayer::setFileName(const QString &filename)
{
    m_fileName = filename;
}

void FFmpegPlayer::stop()
{
    QMutexLocker locker(&m_mutex);
    m_running = false;
    m_paused = false;
    m_pauseCond.wakeAll();  // 唤醒等待中的线程，确保退出
}

void FFmpegPlayer::setVolume(float volume)
{
    m_volume = qBound(0.0f, volume, 1.0f);
    if (m_audioOutput) {
        m_audioOutput->setVolume(m_volume);
    }
}

void FFmpegPlayer::cleanup()
{
    if (m_audioOutput) {
        m_audioOutput->stop();
        delete m_audioOutput;
        m_audioOutput = nullptr;
        m_audioDevice = nullptr;
    }

    if (m_swrCtx) {
        swr_free(&m_swrCtx);
        m_swrCtx = nullptr;
    }
}

void FFmpegPlayer::setupAudio(AVCodecContext *audioCodecCtx)
{
    if (!audioCodecCtx) return;

    // 初始化音频重采样
    m_swrCtx = swr_alloc();
    av_opt_set_int(m_swrCtx, "in_channel_layout", audioCodecCtx->channel_layout, 0);
    av_opt_set_int(m_swrCtx, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(m_swrCtx, "in_sample_rate", audioCodecCtx->sample_rate, 0);
    av_opt_set_int(m_swrCtx, "out_sample_rate", audioCodecCtx->sample_rate, 0);
    av_opt_set_sample_fmt(m_swrCtx, "in_sample_fmt", audioCodecCtx->sample_fmt, 0);
    av_opt_set_sample_fmt(m_swrCtx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(m_swrCtx);

    // 设置音频输出格式
    QAudioFormat format;
    format.setSampleRate(audioCodecCtx->sample_rate);
    format.setChannelCount(2); // 立体声
    format.setSampleSize(16);  // 16位
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    m_audioOutput = new QAudioOutput(format);
    m_audioOutput->setCategory("game");  // 低延迟模式
    m_audioOutput->setBufferSize(3276800);  // 从默认的~8KB增加到32KB
    m_audioOutput->setVolume(m_volume);
    m_audioDevice = m_audioOutput->start();
}

void FFmpegPlayer::processAudioFrame(AVFrame *frame, AVCodecContext *audioCodecCtx)
{
    if (!m_audioDevice || !m_swrCtx) return;

    // 计算输出样本数
    int out_samples = av_rescale_rnd(
        swr_get_delay(m_swrCtx, audioCodecCtx->sample_rate) + frame->nb_samples,
        audioCodecCtx->sample_rate, audioCodecCtx->sample_rate, AV_ROUND_UP);

    // 分配音频缓冲区
    uint8_t *audioBuffer = nullptr;
    av_samples_alloc(&audioBuffer, nullptr, 2, out_samples, AV_SAMPLE_FMT_S16, 0);

    // 重采样
    int samples = swr_convert(m_swrCtx, &audioBuffer, out_samples,
                             (const uint8_t**)frame->data, frame->nb_samples);

    if (samples > 0) {
        int dataSize = av_samples_get_buffer_size(nullptr, 2, samples, AV_SAMPLE_FMT_S16, 1);
        m_audioDevice->write((const char*)audioBuffer, dataSize);
    }

    av_freep(&audioBuffer);
}

void FFmpegPlayer::run()
{
    if (m_fileName.isEmpty()) return;

    m_running = true;

    AVFormatContext *fmtCtx = nullptr;
    AVCodecContext *videoCodecCtx = nullptr, *audioCodecCtx = nullptr;
    SwsContext *swsCtx = nullptr;
    AVFrame *frame = nullptr, *rgbFrame = nullptr, *audioFrame = nullptr;
    AVPacket *packet = nullptr;
    uint8_t *rgbBuf = nullptr;
    int videoStream = -1, audioStream = -1;

    // 打开输入文件
    if (avformat_open_input(&fmtCtx, m_fileName.toUtf8().constData(), nullptr, nullptr) != 0) {
        emit errorOccurred("Failed to open input file");
        goto cleanup;
    }

    if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
        emit errorOccurred("Failed to find stream info");
        goto cleanup;
    }

    m_duration = fmtCtx->duration / (AV_TIME_BASE / 1000);
    emit durationChanged(m_duration);

    // 查找视频流和音频流
    for (unsigned int i = 0; i < fmtCtx->nb_streams; ++i) {
        AVMediaType type = fmtCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStream < 0) {
            videoStream = i;
        } else if (type == AVMEDIA_TYPE_AUDIO && audioStream < 0) {
            audioStream = i;
        }
    }

    if (videoStream == -1 && audioStream == -1) {
        emit errorOccurred("No video or audio stream found");
        goto cleanup;
    }

    // 初始化视频解码器
    if (videoStream != -1) {
        AVCodecParameters *videoCodecPar = fmtCtx->streams[videoStream]->codecpar;
        const AVCodec *videoCodec = avcodec_find_decoder(videoCodecPar->codec_id);
        if (!videoCodec) {
            emit errorOccurred("Video codec not found");
            goto cleanup;
        }

        videoCodecCtx = avcodec_alloc_context3(videoCodec);
        avcodec_parameters_to_context(videoCodecCtx, videoCodecPar);
        if (avcodec_open2(videoCodecCtx, videoCodec, nullptr) < 0) {
            emit errorOccurred("Failed to open video codec");
            goto cleanup;
        }

        // 初始化视频转换
        swsCtx = sws_getContext(videoCodecCtx->width, videoCodecCtx->height, videoCodecCtx->pix_fmt,
                               videoCodecCtx->width, videoCodecCtx->height, AV_PIX_FMT_RGB24,
                               SWS_BILINEAR, nullptr, nullptr, nullptr);

        frame = av_frame_alloc();
        rgbFrame = av_frame_alloc();
        int rgbBufSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, videoCodecCtx->width, videoCodecCtx->height, 1);
        rgbBuf = (uint8_t *)av_malloc(rgbBufSize * sizeof(uint8_t));
        av_image_fill_arrays(rgbFrame->data, rgbFrame->linesize, rgbBuf,
                            AV_PIX_FMT_RGB24, videoCodecCtx->width, videoCodecCtx->height, 1);
    }

    // 初始化音频解码器
    if (audioStream != -1) {
        AVCodecParameters *audioCodecPar = fmtCtx->streams[audioStream]->codecpar;
        const AVCodec *audioCodec = avcodec_find_decoder(audioCodecPar->codec_id);
        if (audioCodec) {
            audioCodecCtx = avcodec_alloc_context3(audioCodec);
            avcodec_parameters_to_context(audioCodecCtx, audioCodecPar);
            if (avcodec_open2(audioCodecCtx, audioCodec, nullptr) == 0) {
                setupAudio(audioCodecCtx);
                audioFrame = av_frame_alloc();
            } else {
                avcodec_free_context(&audioCodecCtx);
                audioCodecCtx = nullptr;
            }
        }
    }

    packet = av_packet_alloc();

    // 主播放循环
    while (m_running && av_read_frame(fmtCtx, packet) >= 0) {
        {
            QMutexLocker locker(&m_mutex);
            if (m_paused) {
                m_pauseCond.wait(&m_mutex);
            }
            if (m_seekPosition >= 0) {
                int64_t seekTarget = m_seekPosition * AV_TIME_BASE / 1000;
                if (av_seek_frame(fmtCtx, -1, seekTarget, AVSEEK_FLAG_BACKWARD) >= 0) {
                    if (videoCodecCtx) avcodec_flush_buffers(videoCodecCtx);
                    if (audioCodecCtx) avcodec_flush_buffers(audioCodecCtx);
                }
                m_seekPosition = -1;
                av_packet_unref(packet);
                continue;
            }
        }

        // 处理视频包
        if (videoCodecCtx && packet->stream_index == videoStream) {
            if (avcodec_send_packet(videoCodecCtx, packet) == 0) {
                while (avcodec_receive_frame(videoCodecCtx, frame) == 0) {
                    // 转换视频帧
                    sws_scale(swsCtx, frame->data, frame->linesize, 0, videoCodecCtx->height,
                             rgbFrame->data, rgbFrame->linesize);

                    // 发送视频帧
                    QImage image(rgbFrame->data[0], videoCodecCtx->width, videoCodecCtx->height,
                                rgbFrame->linesize[0], QImage::Format_RGB888);
                    emit frameReady(image.copy());

                    // 计算并发送当前位置
                    if (frame->pts != AV_NOPTS_VALUE) {
                        qint64 pts_ms = frame->pts * av_q2d(fmtCtx->streams[videoStream]->time_base) * 1000;
                        emit positionChanged(pts_ms);
                    }

                    // 简单帧率控制
                    QThread::usleep(16670); // ~25fps
                }
            }
        }
        // 处理音频包
        else if (audioCodecCtx && packet->stream_index == audioStream) {
            if (avcodec_send_packet(audioCodecCtx, packet) == 0) {
                while (avcodec_receive_frame(audioCodecCtx, audioFrame) == 0) {
                    processAudioFrame(audioFrame, audioCodecCtx);
                }
            }
        }

        av_packet_unref(packet);
    }

cleanup:
    av_packet_free(&packet);
    av_frame_free(&frame);
    av_frame_free(&rgbFrame);
    av_frame_free(&audioFrame);
    av_free(rgbBuf);
    sws_freeContext(swsCtx);
    avcodec_free_context(&videoCodecCtx);
    avcodec_free_context(&audioCodecCtx);
    avformat_close_input(&fmtCtx);
    cleanup();
}



void FFmpegPlayer::pause()
{
    QMutexLocker locker(&m_mutex);
    m_paused = true;
}

void FFmpegPlayer::resume()
{
    QMutexLocker locker(&m_mutex);
    m_paused = false;
    m_pauseCond.wakeAll();
}

void FFmpegPlayer::seekTo(qint64 ms)
{
    QMutexLocker locker(&m_mutex);
    m_seekPosition = ms;
    qDebug()<<"seek to:"<<ms<<"ms";
}

