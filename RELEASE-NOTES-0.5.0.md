# Unified 0.5.0

Android ARM64, API 31+. This is the 0.4.107 candidate rebuilt with release version 0.5.0. It includes the Vulkan 1.4 Lab configuration. Vulkan conformance remains uncertified.

## Custom sampler border colors

Unified now exposes `VK_EXT_custom_border_color`. Mali-G57 and Mali-G615 support application-defined border colors with or without an explicit image-view format. Mali-G52 requires an explicit format and applies the Bifrost AFBC channel order. The driver reports a limit of 4,000 custom-border samplers.

On RG477M, the 0.4.107 candidate removed the dark polygons in Conker: Live & Reloaded at both 1× and 2× with the original BoxForever 1.0.4 renderer. The character's shadow remained visible, as on the stock Vulkan driver.

| Device | Unified targeted CTS | Stock Vulkan targeted CTS |
| --- | --- | --- |
| RG477M | 21 pass, 0 fail, 21 unsupported | 21 pass, 0 fail, 21 unsupported |
| RG476H | 24 pass, 0 fail, 21 unsupported | 24 pass, 0 fail, 21 unsupported |
| RG Rotate | 12 pass, 6 fail, 27 unsupported | 24 pass, 0 fail, 21 unsupported |

The RG476H and Rotate totals include three additional BGR cases. On Rotate the failures affect RGB565 and BGR custom colors. Compared with stock Vulkan, six additional graphics cases are skipped because Unified exposes the extension on Mali-G52 only with an explicit format. The original BoxForever renderer asks for the formatless feature and can still use its approximate border color on this GPU. This release is marked as a pre-release while this limitation remains open.

## Installation

Import `Unified-0.5.0-Android-API31.zip` through your application's custom GPU driver settings, select Unified 0.5.0, and restart the game. The release tag contains the driver sources and the patch for the pinned Mesa revision.

## Русский

Unified 0.5.0 — сборка кандидата 0.4.107 с новым номером для Android ARM64, API 31 и новее. Используется лабораторная конфигурация Vulkan 1.4. Сертификация соответствия Vulkan не пройдена.

Добавлен `VK_EXT_custom_border_color`. На Mali-G57 и Mali-G615 приложение может задать цвет с форматом и без него. На Mali-G52 формат нужно указывать. Исходный BoxForever может по-прежнему использовать приблизительный цвет границы на G52, поскольку запрашивает вариант без формата.

На RG477M кандидат 0.4.107 устранил тёмные многоугольники в Conker при 1× и 2× на исходном BoxForever 1.0.4. Обычная тень персонажа сохранилась. На RG477M оба драйвера прошли 21 графический тест CTS, на RG476H — 24. На Rotate Unified прошёл 12 тестов и получил шесть ошибок RGB565 и BGR. У штатного Vulkan на Rotate прошли все 24 графических теста. Сборка опубликована как предварительная из-за этой регрессии.
