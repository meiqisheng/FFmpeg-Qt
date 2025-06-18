#ifndef WIDGET_H
#define WIDGET_H

#include "ffmpegplayer.h"
#include <QWidget>

QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

private slots:
    void openVideo();
    void playVideo();
    void stopVideo();
    void pauseVideo();
    void resumeVideo();
    void displayFrame(const QImage &image);

private:
    Ui::Widget *ui;
    FFmpegPlayer *player;
};
#endif // WIDGET_H
