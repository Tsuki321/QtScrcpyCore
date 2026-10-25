#ifndef INPUTCONVERT_H
#define INPUTCONVERT_H

#include <QTimer>

#include "inputconvertbase.h"

class InputConvertNormal : public InputConvertBase
{
    Q_OBJECT
public:
    InputConvertNormal(Controller *controller);
    virtual ~InputConvertNormal();

    virtual void mouseEvent(const QMouseEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void wheelEvent(const QWheelEvent *from, const QSize &frameSize, const QSize &showSize);
    virtual void keyEvent(const QKeyEvent *from, const QSize &frameSize, const QSize &showSize);

private:
    void sendTouchEvent(AndroidMotioneventAction action, QRect pos, AndroidMotioneventButtons actionButton, AndroidMotioneventButtons buttons, float pressure);
    void flushPendingMove();

    AndroidMotioneventButtons convertMouseButtons(Qt::MouseButtons buttonState);
    AndroidMotioneventButtons convertMouseButton(Qt::MouseButton button);
    AndroidKeycode convertKeyCode(int key, Qt::KeyboardModifiers modifiers);
    AndroidMetastate convertMetastate(Qt::KeyboardModifiers modifiers);

    bool m_hasPendingMove = false;
    QRect m_pendingMovePos;
    AndroidMotioneventButtons m_pendingMoveButtons;
    QTimer m_moveFlushTimer;
    QSize m_pendingMoveFrameSize;
};

#endif // INPUTCONVERT_H
