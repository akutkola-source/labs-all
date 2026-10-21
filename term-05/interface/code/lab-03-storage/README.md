# ЛР №3 «Устройства хранения данных» — Вариант А4
## Сравнительный анализатор HDD vs SSD (Windows, C++, Win32 API)

### Сборка и запуск
- Visual Studio: открыть `HddSsdCompare.vcxproj`, конфигурация Release x64, собрать.
- Командная строка: `build.bat`, готовый файл — `bin\HddSsdCompare.exe`.
- Программа запускается с обычными правами. Для SMART SATA-дисков (ATA pass-through) нужны права администратора: нажмите «Перезапустить от администратора» или запустите exe через «Запуск от имени администратора».
- Режим без окна: `HddSsdCompare.exe /report отчёт.html` (или `.txt`).

### Что реализовано (требования варианта)
| Требование | Реализация |
|---|---|
| Автоматическое определение типа HDD/SSD | `StorageDeviceSeekPenaltyProperty`, `StorageDeviceTrimProperty`, скорость вращения (Nominal Media Rotation Rate, IDENTIFY word 217), шина NVMe |
| Таблица сравнения двух дисков | Объём, интерфейс (+ скорость SATA-канала), скорость вращения (HDD) / TRIM (SSD), температура, часы работы, износ, резерв, переназначенные секторы, SMART-статус, тома и др.; столбец «Лучше» и цветовая подсветка |
| Рекомендации по замене | Износ > 90 % — замена (> 80 % — предупреждение); температура > 75 °C — перегрев (> 60 °C — предупреждение); AvailableSpare < 10 %; NVMe Critical Warning; SMART-порог превышен; переназначенные/нестабильные секторы; наработка HDD > 40 000 ч; заполнение тома > 90 % |
| Экспорт сравнительного отчёта | Кнопка «Экспорт отчёта…» → HTML (таблицы с подсветкой) или TXT |

Дополнительно (из базового задания): список всех физических дисков с томами и заполнением, журнал событий,
автообновление по таймеру (10 с), автоматическое обновление при подключении/отключении дисков (`WM_DEVICECHANGE`).

### Используемые системные интерфейсы (без WMI/.NET/сторонних библиотек)
| Задача | API |
|---|---|
| Перечисление дисков | `SetupDiGetClassDevs(GUID_DEVINTERFACE_DISK)` + `IOCTL_STORAGE_GET_DEVICE_NUMBER` |
| Модель, прошивка, серийный номер, шина | `IOCTL_STORAGE_QUERY_PROPERTY` / `StorageDeviceProperty` |
| Признаки SSD | `StorageDeviceSeekPenaltyProperty`, `StorageDeviceTrimProperty` |
| Объём, размер сектора | `IOCTL_DISK_GET_DRIVE_GEOMETRY_EX` |
| Температура | `StorageDeviceTemperatureProperty`; запасные варианты — SMART 0xC2/0xBE, NVMe Health Log |
| SATA: IDENTIFY, SMART | `IOCTL_ATA_PASS_THROUGH`: 0xEC, 0xB0/0xD0 (READ DATA), 0xB0/0xD1 (THRESHOLDS), 0xB0/0xDA (RETURN STATUS) |
| NVMe: здоровье | `StorageDeviceProtocolSpecificProperty`, `NVME_LOG_PAGE_HEALTH_INFO` (Log Page 0x02) |
| Тома и заполнение | `GetLogicalDrives`, `IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS`, `GetDiskFreeSpaceEx` |

Свойства `StorageDeviceRPMProperty` в Windows SDK нет, поэтому скорость вращения читается из слова 217 ответа IDENTIFY DEVICE:
1 — не вращается (SSD), 0x0401…0xFFFE — обороты в минуту, 0 — диск не сообщает значение.

Алгоритм определения типа: NVMe → SSD; иначе RPM (1 → SSD, > 1 → HDD); иначе SeekPenalty (нет → SSD, есть → HDD); иначе TRIM.

Износ SATA SSD берётся из нормализованного значения атрибута ресурса: 0xE7, 0xA9, 0xB1, 0xE9, 0xCA или 0xAD
(износ = 100 − текущее значение). Для NVMe используется поле PercentageUsed.

### Проверка
Проверено на ПК с одним NVMe SSD (HOGE H8A1 512GB): тип определён как SSD, температура, часы работы,
износ, резерв и объём прочитанных и записанных данных получены, отчёт формируется (`пример_отчёта.html`, `screenshot.png`).
Ветку SATA/HDD (ATA pass-through, SMART-атрибуты, RPM) на этом ПК проверить нельзя: в нём нет SATA-накопителя.
