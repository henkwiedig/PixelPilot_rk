#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Launch /usr/bin/bind in the background and show a modal "Binding ..."
// popup tracking its result ("Binding ... success !!!" on exit code 0,
// "Binding ... failed !!!" otherwise). The popup's Cancel button kills the
// process; once it finishes the same button relabels to "Close". No-op if a
// bind is already in progress.
void bind_dialog_trigger(void);

#ifdef __cplusplus
}
#endif
