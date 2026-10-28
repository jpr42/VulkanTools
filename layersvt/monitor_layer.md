<!-- markdownlint-disable MD041 -->
<p align="left"><img src="https://vulkan.lunarg.com/img/NewLunarGLogoBlack.png" alt="LunarG" width=263 height=113 /></p>
<p align="left">Copyright &copy; 2015-2026 LunarG, Inc.</p>

[![Creative Commons][3]][4]

[3]: https://i.creativecommons.org/l/by-nd/4.0/88x31.png "Creative Commons License"
[4]: https://creativecommons.org/licenses/by-nd/4.0/

# VK\_LAYER\_LUNARG\_monitor
The `VK_LAYER_LUNARG_monitor` utility layer draws a small overlay in the corner of every image the application presents, showing:

- the frame rate in frames per second
- the average frame time in milliseconds, with the shortest and longest frame time in parentheses
- the frame number

The overlay is drawn into the presented images, so it works in fullscreen and doesn't depend on the window system. The application's window title is left alone.

The frame rate and frame times are measured between presents of each swapchain, and refresh twice a second. The frame number updates every frame.

For an overview of how to configure layers, refer to the [Layers Overview and Configuration](https://vulkan.lunarg.com/doc/sdk/latest/windows/layer_configuration.html) document.

The Monitor Layer can be enabled using the [Vulkan Configurator](https://vulkan.lunarg.com/doc/sdk/latest/windows/vkconfig.html) included with the Vulkan SDK.

## Using the Monitor Layer with RenderDoc

The frame number starts at 0 for the first presented image and counts every `vkQueuePresentKHR` call of the `VkInstance`. It matches the frame number RenderDoc gives a capture of that frame, so it can be used to choose which frame to capture. RenderDoc's own overlay shows a number one higher.

Load the Monitor Layer below RenderDoc. Otherwise RenderDoc captures the work the Monitor Layer does to draw the overlay.

## Limitations

The overlay isn't drawn, and the layer prints a message once, for:

- presents from a queue family without graphics support
- swapchains with a shared present mode
- protected swapchains
- swapchains created with `vkCreateSharedSwapchainsKHR`

Applications keep working as they would without the layer in these cases.

To draw into the swapchain images, the layer adds `VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT` to the swapchain image usage.
