// A system dialog while the renderer runs GXM (platform/sys_vita.cpp). GXM
// starts once in a process, so the dialog cannot make a display of its own
// then: it draws through the renderer's.
#ifndef OPENCMR2_GXM_DIALOG_H
#define OPENCMR2_GXM_DIALOG_H

struct GxmDialogPresenter {
    virtual ~GxmDialogPresenter() {}
    // Ends what the renderer has open and waits for the GPU. The frame the
    // game was recording continues after the dialog, without what it drew
    // before.
    virtual void BeginDialog() = 0;
    // One frame: the dialog over black, shown at the next vertical blank.
    virtual void DialogFrame() = 0;
};

// The renderer's presenter. nullptr: the renderer has not started GXM.
GxmDialogPresenter *Gxm_DialogPresenter(void);
void Gxm_SetDialogPresenter(GxmDialogPresenter *presenter);

#endif
