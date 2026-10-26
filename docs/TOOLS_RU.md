# Инструменты и утилиты

Для записи готового релиза нужны Windows 10/11, PowerShell 5.1 или 7, FT232 с
питанием и уровнями 3,3 V. Python 3.10+ нужен для независимой проверки readback.
Для сборки добавляется Arm GCC; для C-тестов — Clang. Git нужен при клонировании,
GitHub CLI используется издателем, но не требуется пользователю публичного релиза.

## Закреплённые пакеты

| Инструмент | Версия / назначение | Загрузка |
|---|---|---|
| Arm GNU Toolchain | 15.3.Rel1, GCC 15.3.1, Cortex-M0+ | `tools/fetch_tools.ps1` |
| LLVM-MinGW | 20261020 UCRT x86_64, Clang 23.1.4, C-тесты | `tools/prepare_host_tests.ps1` |
| XHSC MCU Programmer | V2.23, ROM ISP по FT232 | `tools/fetch_tools.ps1 -ProgrammerOnly` |
| HC32F005 DDL | Rev2.0.0, необходимые исходники периферии | Уже в `vendor/` |
| Arm CMSIS | 5.9.0, заголовки ядра | Уже в `vendor/` |

Компиляторы закреплены для воспроизводимости подтверждённого HEX/BIN; их установка
не меняет PATH системы. Программатор скачивается из публичного вложения проекта.
Драйвер FT232 при необходимости устанавливается отдельно с
[официальной страницы FTDI VCP](https://ftdichip.com/drivers/vcp-drivers/).

| Пакет | Размер, байт | SHA-256 |
|---|---:|---|
| arm-gnu-toolchain-15.3.rel1-mingw-w64-i686-arm-none-eabi.zip | 292045003 | `f1941b026f7a5383cf69ad09640dd4ef3e1479d612fd77c2c24cd820cf5c279d` |
| llvm-mingw-20261020-ucrt-x86_64.zip | 190612575 | `fd5462b7f5d93b2ff981fcd3f9c7e427e47f3203c64ea91c27bcc444eebd74cd` |
| XHSC_ISP_V2.23.zip | 729856 | `2f729500a72fce0e6eacf5be018c2489419bd2cc0623c02a196837bb441501ae` |

Сам XHSC.exe дополнительно проверяется перед записью:
`c20ca3d4c321e78d2d8059281e2f133821cab0627aaa39ad2021389f3941ff30`.

## Скрипты

| Файл | Действие |
|---|---|
| `fetch_tools.ps1` | ARM; `-IncludeProgrammer` добавляет XHSC, `-ProgrammerOnly` скачивает только XHSC; `-VerifyOnly` проверяет уже загруженные архивы |
| `prepare_host_tests.ps1` | Загрузка, проверка и подготовка Clang; `-VerifyOnly` проверяет архив и установленное содержимое |
| `build.ps1` | Сборка HEX/BIN/ELF/MAP в папку версии из config.h |
| `test.ps1` | Девять C-наборов с моделью периферии |
| `audit.py` | Статический аудит построенных HEX/BIN/ELF/MAP, векторов и границы журнала |
| `check_hex_storage.ps1` | Синтаксис/контрольные суммы HEX и запрет записи в журнал |
| `flash.ps1` | Page erase/blank/program/verify через закреплённый XHSC; `-DryRun` не открывает COM |
| `open_programmer.ps1` | Открывает настроенную GUI в отдельной сессии с правильной рабочей папкой; `-Readback` готовит чтение всей Flash, `-PrepareOnly` не открывает GUI/COM |
| `package_release.py` | Издательский комплект из чистого Git: только отслеживаемые файлы, закреплённые firmware/tool и SHA-256 |
| `verify_readback.py` | Независимая проверка считанных HEX/BIN, побайтового совпадения приложения, padding и CRC журнала; COM не использует |

`open_programmer.ps1` не нажимает Upload или Execute. Рабочая папка задаётся явно,
чтобы XHSC сохранял Config.xml рядом со своей сессионной копией, а не в системной
папке запуска. MCU/скорость закреплены: family HC32L110x6xx/HC32F005x6xx,
index 33, baud 115200/index 3; Flash 32 KiB, страницы 512 байт.

Все сессии, readback и отчёты платы храните в ignored `hardware-local/`.
Подробные действия — [FLASHING_RU.md](FLASHING_RU.md).
