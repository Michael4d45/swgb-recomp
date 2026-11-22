/* What the host does when lifted code reaches code that was not lifted. */
#pragma once

/* Install the miss handler. Call once, after native32_init(). */
void coverage_init(void);
