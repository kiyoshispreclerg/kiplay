/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef KP_I18N_H
#define KP_I18N_H

/* gettext. The msgids are the English texts; the translations live in po/.
 * Everything a person reads goes through _(): the window, the text modes
 * (--list...) and the log messages. */

#include <libintl.h>

#define KP_GETTEXT_DOMAIN "kiplay"

#define _(s) gettext(s)
/* Marks a string to be extracted now and translated later, where it is used. */
#define N_(s) (s)

#endif
