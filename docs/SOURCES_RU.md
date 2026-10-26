# Источники

- [rcambrj/sovol-dryer-firmware](https://github.com/rcambrj/sovol-dryer-firmware/tree/5852d073c92a69590cb5c14f0f5c4608388fb6a3)
  — сведения о заводском управлении, HC32, подключении и готовые HEX.
  C-исходников приложения в проверенном дереве нет.
- [AlexStOd/Sovol3D](https://github.com/AlexStOd/Sovol3D/tree/245823422bc10dcd57a76435bd5f9804d8278c05)
  — C под STM8, сведения о меню, LCD и датчике. Это поведенческий источник,
  не готовая прошивка под HC32. Существенные части кода не копировались;
  приложение проекта написано самостоятельно.
- [HC32F005 DDL Rev2.0.0](https://oss-nc-beijing-2.cecloudcs.com/doc-hc/HC32F005_DDL_Rev2.0.0.zip)
  — официальная библиотека периферии XHSC. В vendor включены только необходимые файлы.
- [Arm CMSIS 5.9.0](https://github.com/ARM-software/CMSIS_5/tree/5.9.0)
  — заголовки Cortex-M0+; лицензия Apache-2.0, исходные уведомления сохранены.
- [Arm GNU Toolchain](https://developer.arm.com/Tools%20and%20Software/GNU%20Toolchain)
  — компилятор; закреплён пакет 15.3.Rel1 для Windows.

Собственный код распространяется по LICENSE.txt. Файлы vendor сохраняют
свои уведомления: XHSC разрешает использование с XHSC MCU; CMSIS — Apache-2.0.
Подлинный C-код заводского приложения HC32 отсутствует; для сборки бинарник
заводской прошивки не требуется.
