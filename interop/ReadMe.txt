
This example demonstrates sandboxed async renderer process generating
sharing frames for master process via OpenGL interop.

This example uses WGL_NV_DX_interop2.

When you run the example, you'll see a first window that reports:

"Waiting for the renderer process to start..."

After a brief pause, the "interop master (VR app)" window (LEFT) and
"interop renderer (VR webview sandbox)" window (RIGHT) will both appear
and begin animating.

The LEFT window should refresh at a constant 60 frames per second
while showing a repeated texturing of the most recent rendered image
in the RIGHT window.  The textured image "rocks back and forth" to make
it clear that the LEFT window is constantly rendering.

The RIGHT window renders uses a timer to render once per second
(initially), but you can increase/decrease its timed rendering rate with
the +/- keys.

If you "hold down" the minus key, you can watch the rendering rate of the RIGHT
window go to 10 milliseconds and the wireframe sphere show will rotate
very fast.

The text drawn in the RIGHT window (and shown mirrored in the LEFT window)
is drawn with NV_path_rendering.

- Mark Kilgard
  January 12, 2017
