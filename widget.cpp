#include "widget.h"
#include "ui_widget.h"
#include <QFileDialog>
#include <QDebug>

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);
    player = new FFmpegPlayer(this);
    connect(player, &FFmpegPlayer::frameReady, this, &Widget::displayFrame);
    connect(ui->Open, &QPushButton::clicked, this, &Widget::openVideo);
    connect(ui->Play, &QPushButton::clicked, this, &Widget::playVideo);
    connect(ui->Pause, &QPushButton::clicked, this, &Widget::pauseVideo);
    connect(ui->Resume, &QPushButton::clicked, this, &Widget::resumeVideo);
    connect(ui->Stop, &QPushButton::clicked, this, &Widget::stopVideo);
    connect(player, &FFmpegPlayer::durationChanged, this, [=](qint64 duration) {
        ui->VideoSlider->setRange(0, duration);
    });

    connect(player, &FFmpegPlayer::positionChanged, this, [=](qint64 position) {
        if (!ui->VideoSlider->isSliderDown())  // 如果用户正在拖动，不更新
            ui->VideoSlider->setValue(position);
    });

    connect(ui->VideoSlider, &QSlider::sliderReleased, this, [=]() {
        player->seekTo(ui->VideoSlider->value());
    });

    // 音量控制
    ui->VolumeSlider->setRange(0, 100);
    ui->VolumeSlider->setValue(80);
    connect(ui->VolumeSlider, &QSlider::valueChanged, this, [=](int value) {
        player->setVolume(value / 100.0f);
    });
    // 固定窗口大小为当前大小，禁用最大化和缩放
    this->setFixedSize(this->size());
}

Widget::~Widget()
{
    player->stop();
    player->wait();
    delete ui;
}

void Widget::openVideo()
{
    QString filename = QFileDialog::getOpenFileName(this, "Open Video File");
    if (!filename.isEmpty()) {
        player->setFileName(filename);
    }
}

void Widget::playVideo()
{
    if (!player->isRunning()) {
        player->start();
    }
}

void Widget::displayFrame(const QImage &image)
{
    // 按 QLabel 大小缩放图像（保持比例或不保持比例都可以）
    QPixmap pixmap = QPixmap::fromImage(image.scaled(ui->VideoFrame->size(),
                                                    Qt::KeepAspectRatio,  // 保持比例
                                                    Qt::SmoothTransformation));
    ui->VideoFrame->setPixmap(pixmap);
}

void Widget::pauseVideo()
{
    player->pause();
}

void Widget::resumeVideo()
{
    player->resume();
}

void Widget::stopVideo()
{
    player->stop();
    player->wait();
    ui->VideoFrame->clear();
}
