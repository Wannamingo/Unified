# SPDX-License-Identifier: MIT
"""Generate the maintenance5 lost-device dispatch layer from vk.xml."""

import sys
from pathlib import Path
from xml.etree import ElementTree


def main():
    xml, output = map(Path, sys.argv[1:])
    wrappers = []
    slots = []
    for command in ElementTree.parse(xml).findall('./commands/command'):
        if 'VK_ERROR_DEVICE_LOST' not in command.get('errorcodes', '').split(','):
            continue
        owner = command.findtext('./param/type')
        if owner not in ('VkDevice', 'VkQueue', 'VkCommandBuffer'):
            continue
        if owner == 'VkCommandBuffer':
            raise ValueError('Handle conversion needed for command-buffer dispatch')
        assert command.findtext('./proto/type') == 'VkResult'
        name = command.findtext('./proto/name')[2:]
        params = [param for param in command.findall('param')
                  if 'vulkan' in param.get('api', 'vulkan').split(',')]
        declaration = ', '.join(''.join(param.itertext()) for param in params)
        arguments = ', '.join(param.findtext('name') for param in params)
        handle = params[0].findtext('name')
        dev = (f'mali_device_from_handle({handle})' if owner == 'VkDevice'
               else f'container_of(mali_queue_from_handle({handle})->vk.base.device, struct mali_device, vk)')
        wrappers.append(f'''static VKAPI_ATTR VkResult VKAPI_CALL
lost_{name}({declaration})
{{
   struct mali_device *dev = {dev};
   if (vk_device_is_lost_no_report(&dev->vk))
      return VK_ERROR_DEVICE_LOST;
   VkResult result;''')
        if name == 'GetEventStatus':
            wrappers.append('''#if UNIFIED_VALIDATION_LAB
   if (p_atomic_cmpxchg(&dev->test_lose_on_event_status, 1, 0) == 1)
      result = VK_ERROR_DEVICE_LOST;
   else
#endif''')
        call = (f'wait_semaphores_with_loss(dev, pWaitInfo, timeout)' if name == 'WaitSemaphores'
                else f'dev->cmd_dispatch.{name}({arguments})')
        wrappers.append(f'''      result = {call};
   if (unlikely(result == VK_ERROR_DEVICE_LOST))
      mali_device_set_lost(dev, "vk{name} returned VK_ERROR_DEVICE_LOST");
   return result;
}}
''')
        slots.append(f'''   if (dispatch->{name})
      dispatch->{name} = lost_{name};''')
    assert len(slots) >= 20
    wrappers.append('''static void
install_device_loss_dispatch(struct vk_device_dispatch_table *dispatch)
{''')
    output.write_text('\n'.join(wrappers + slots + ['}', '']), encoding='utf-8')


if __name__ == '__main__':
    main()
