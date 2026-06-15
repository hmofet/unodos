/* uno3d demo entry points (uno3d_demo.c) - shared by every platform's glue. */
#ifndef UNO3D_DEMO_H
#define UNO3D_DEMO_H

void demo_init(int w, int h);   /* set the viewport aspect */
void demo_frame(float t);       /* render one frame; t = elapsed seconds */

#endif
