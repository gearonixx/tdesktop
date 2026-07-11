# Разбор коммита `f0dc7d4850` — «Fixed new-login alert bar layout in collapsed chats list»

**Автор:** gearonixx · **Дата:** 2026-07-03 · **Ветка:** `test/collapsed-chats-alert-bar`
**Родитель:** `110d4f5` · **Диапазон:** 1 файл, +6 −1
**Файл:** `Telegram/SourceFiles/dialogs/ui/dialogs_top_bar_suggestion_content.cpp`
**Отчёт составлен:** 2026-07-10

---

## TL;DR

Коммит чинит **две независимые проблемы** в методе `UnconfirmedAuthWrap::resizeGetHeight(int newWidth)` —
это метод, определяющий высоту плашки-алерта «новый вход в аккаунт», которая висит сверху списка чатов:

1. **Дефект вёрстки в узкой колонке.** Плашка не уважала общесистемный порог «narrow»-режима
   (`columnMinimalWidthLeft / 2` = 130px) и пыталась разложить свой контент по слишком малой ширине,
   раздувая высоту и ломая раскладку. Фикс: ниже порога возвращать высоту `0` (полностью прятаться),
   как это уже делают все соседние бары (`requests_bar`, `group_call_bar`, `more_chats_bar`).

2. **Рассинхрон высоты во время анимации сворачивания.** «Живой» путь расчёта высоты (без снапшота)
   игнорировал `_collapseProgress`, тогда как путь со снапшотом его учитывал. Из-за этого при ре-лейауте
   в середине анимации высота скакала на полную. Фикс: применять `* (1. - _collapseProgress)` в обоих
   путях — как это уже сделано в соседнем классе `TopBarSuggestionContent::resizeGetHeight`.

Оба фикса — по сути **приведение поведения плашки к уже принятым в кодовой базе конвенциям**.
Ничего нового не изобретается; закрываются два места, где `UnconfirmedAuthWrap` отставал от своих «братьев».

---

## 1. Контекст: что это за виджет и как он живёт

### 1.1 Что за плашка
`UnconfirmedAuthWrap` (объявлен в `dialogs_top_bar_suggestion_content.h:30`) — это один из «top bar suggestions»,
всплывающих над списком диалогов. Конкретно — **алерт о неподтверждённом входе** (`UnreviewedAuth`):
когда в аккаунт кто-то залогинился с нового устройства, сверху списка чатов появляется скруглённая
капсула («pill») с текстом вида «Вход с устройства X из страны Y» и кнопками подтвердить/отклонить.

Наследование:
```cpp
class UnconfirmedAuthWrap : public Ui::SlideWrap<Ui::VerticalLayout>
```
`SlideWrap` даёт анимацию показа/скрытия по высоте; `VerticalLayout` внутри — сам контент капсулы.
Отрисовка pill (тень, фон, скругление, «блик» сверху на тёмных темах) идёт в `content->paintOn(...)`
в `CreateUnconfirmedAuthContent` — но к геометрии/высоте это отношения не имеет, важен только
`resizeGetHeight`.

### 1.2 Механика сворачивания (`_collapseProgress`)
Поле `float64 _collapseProgress = 0.` — прогресс сворачивания: `0.` = полностью развёрнута,
`1.` = свёрнута в ноль высоты.

Откуда он берётся (`dialogs_top_bar_suggestion.cpp` + `suggestion_unreviewed_auth.cpp:117,130`):
```cpp
wrap->setCollapseProgress(args.context.childListShown());   // = _childListShown
```
То есть `_collapseProgress` **привязан к `childListShown`** — прогрессу показа «дочернего» списка
(forum-топиков / child-list), который в узком одноколоночном режиме **наезжает поверх** списка диалогов.
Когда `childListShown` едет `0. → 1.`, плашка-алерт должна плавно схлопнуться (её место занимает
въезжающий child-list). `_childListShown` живёт в `dialogs_widget.cpp` (`:394, :701, :3494` и т.д.).

### 1.3 Снапшот-заморозка во время анимации
Чтобы во время анимации не пере-раскладывать живой контент на каждый кадр, введён механизм снапшота
(коммит-предшественник `1c69103fc8` «Froze top bar suggestion content during child-list collapse animation»):

- `prepareCollapseSnapshot()` — вызывается в начале сворачивания (`suggestion_unreviewed_auth.cpp:131`
  через `args.done(wrap, [wrap]{ wrap->prepareCollapseSnapshot(); })`). Делает `Ui::GrabWidget(this)`
  в `_collapseSnapshot` (QPixmap) и прячет живой `wrapped()`. Дальше `paintRequest` рисует пиксмап.
- `releaseCollapseSnapshot()` — вызывается когда `_collapseProgress` вернулся в `0.`
  (см. `setCollapseProgress`): обнуляет пиксмап и снова показывает живой контент.

Таким образом у `resizeGetHeight` **два режима**:
- **со снапшотом** (`!_collapseSnapshot.isNull()`) — высота считается от высоты пиксмапа;
- **живой** — высота считается от `wrapped()->height()` после `resizeToWidth`.

Именно расхождение этих двух путей и породило дефект №2.

---

## 2. Диффы построчно

### 2.1 Новый `#include`
```diff
 #include "styles/style_settings.h"
+#include "styles/style_window.h"
```
Нужен ради константы `st::columnMinimalWidthLeft` (определена в `window/window.style:20` = `260px`).
Соседние бары включают её точно так же, часто с комментарием: см.
`ui/chat/more_chats_bar.cpp:16`, `ui/chat/requests_bar.cpp:18`, `ui/chat/group_call_bar.cpp:20`
(`#include "styles/style_window.h" // st::columnMinimalWidthLeft`).

### 2.2 Ранний выход по ширине (фикс №1)
```diff
 int UnconfirmedAuthWrap::resizeGetHeight(int newWidth) {
+	if (newWidth < st::columnMinimalWidthLeft / 2) {
+		return 0;
+	}
 	if (!_collapseSnapshot.isNull()) {
```

### 2.3 Учёт прогресса в живом пути (фикс №2)
```diff
 	if (const auto w = wrapped()) {
 		w->resizeToWidth(newWidth);
 	}
-	return wrapped() ? wrapped()->height() : 0;
+	const auto fullHeight = wrapped() ? wrapped()->height() : 0;
+	return int(base::SafeRound(fullHeight * (1. - _collapseProgress)));
 }
```

Итоговый метод целиком:
```cpp
int UnconfirmedAuthWrap::resizeGetHeight(int newWidth) {
	if (newWidth < st::columnMinimalWidthLeft / 2) {
		return 0;
	}
	if (!_collapseSnapshot.isNull()) {
		const auto fullHeight = int(_collapseSnapshot.height()
			/ _collapseSnapshot.devicePixelRatio());
		return int(base::SafeRound(fullHeight * (1. - _collapseProgress)));
	}
	if (const auto w = wrapped()) {
		w->resizeToWidth(newWidth);
	}
	const auto fullHeight = wrapped() ? wrapped()->height() : 0;
	return int(base::SafeRound(fullHeight * (1. - _collapseProgress)));
}
```

---

## 3. Проблема №1 — плашка ломала вёрстку в узкой колонке

### 3.1 Что происходило
До фикса, при любой ширине, живой путь делал:
```cpp
w->resizeToWidth(newWidth);
return wrapped()->height();
```
У капсулы фиксированные `st::dialogsTopBarSuggestionMargins` + `st::dialogsUnconfirmedAuthPadding`.
Когда левую колонку сужают до узкого/иконочного режима (ширина → десятки пикселей), доступной ширины
под текст почти не остаётся: текст переносится на много строк, высота `wrapped()->height()` раздувается,
а сама pill получается уродливой (или `pill.isEmpty()` в paint-коллбэке). Результат — **сломанная,
переросшая по высоте плашка в свёрнутом списке чатов** (это ровно то, что вынесено в заголовок коммита).

### 3.2 Почему порог именно `columnMinimalWidthLeft / 2` (= 130px)
Это **не магическое число, а общесистемная граница narrow-режима**. По всей кодовой базе `260/2 = 130px`
используется как переключатель «узкий/иконочный» режим списка диалогов:

| Файл | Строка | Использование |
|------|--------|---------------|
| `dialogs/dialogs_inner_widget.cpp` | 865, 1290, 1368, 1490 | `.narrow = (fullWidth < st::columnMinimalWidthLeft / 2)` |
| `ui/chat/requests_bar.cpp` | 154 | `if (width >= st::columnMinimalWidthLeft / 2)` (иначе не рисует текст) |
| `ui/chat/group_call_bar.cpp` | 257, 274 | `narrow = (outerWidth < st::columnMinimalWidthLeft / 2)` |
| `mainwidget.cpp` | 2618 | `(newWidth < st::columnMinimalWidthLeft / 2)` при расчёте ratio |

То есть фикс №1 **приводит алерт-плашку к тому же порогу**, по которому уже переключаются все остальные
элементы списка. Раньше `UnconfirmedAuthWrap` был исключением, который этот порог игнорировал.

### 3.3 Почему `return 0`, а не «сжать текст»
Соседи ведут себя двумя способами: `requests_bar` просто **не рисует текст** ниже порога (оставляя
только юзерпики), а `group_call_bar` переключается в компактный `narrow`-вид. У алерт-плашки нет
осмысленного «иконочного» представления (это капсула с длинным текстом и кнопками), поэтому самое
чистое — **полностью скрыться** (высота 0). В узком режиме место всё равно отдаётся въезжающему
child-list, так что скрытие визуально корректно.

---

## 4. Проблема №2 — рассинхрон высоты в анимации сворачивания

### 4.1 Что происходило
Путь со снапшотом уже был правильным:
```cpp
return int(base::SafeRound(fullHeight * (1. - _collapseProgress)));  // высота тает 0..1
```
А живой путь возвращал **полную** `wrapped()->height()`, игнорируя `_collapseProgress`.

Пока анимация идёт «штатно» через снапшот — проблемы не видно. Но снапшот существует не всегда:
- между `prepareCollapseSnapshot()` и первым кадром;
- если по какой-то причине снапшот не был снят / уже освобождён, а `_collapseProgress` ещё > 0;
- при внешнем ре-лейауте (например, ресайз окна) в середине анимации, когда движок дёргает
  `resizeGetHeight` на живом пути.

В такие моменты высота **скакала обратно на полную** вместо плавно уменьшенной → визуальный «прыжок»
вёрстки списка в момент, когда чат наезжает поверх диалогов.

### 4.2 Эталон — соседний класс
Что фикс просто копирует уже готовый паттерн, видно по `TopBarSuggestionContent::resizeGetHeight`
(тот же файл, ~line 587) — это «брат» для обычных top-bar-подсказок:
```cpp
const auto withMargins = capped + rect::m::sum::v(margins);
return int(base::SafeRound(withMargins * (1. - _collapseProgress)));
```
Здесь `* (1. - _collapseProgress)` применялся **всегда**. `UnconfirmedAuthWrap` был единственным местом,
где на живом пути этого множителя не было. Фикс №2 **устраняет это единственное расхождение** —
теперь оба класса и оба пути (`snapshot`/live) считают высоту по одной формуле.

### 4.3 Корректность на границах
- `_collapseProgress == 0.` → множитель `1.` → полная высота (как раньше). ✔
- `_collapseProgress == 1.` → `0` высоты (полностью свёрнуто). ✔
- `base::SafeRound` (`lib_base/base/algorithm.h:132`) даёт корректное округление до int без UB. ✔

---

## 5. Место в серии коммитов

Файл активно допиливался последними ~12 коммитами (редизайн top-bar-подсказок в «pill»-стиль):
```
f0dc7d4850  Fixed new-login alert bar layout in collapsed chats list.   <-- этот
595e235981  Replaced unconfirmed auth bar text snapshot with producer.
fce19ac336  Aligned userpic and text in top bar suggestion with dialog row column.
79fb35cc15  Added top-edge sheen to top bar suggestion pill on dark themes.
95369274bb  Reserved close-button width on every text line of top bar suggestion.
...
1c69103fc8  Froze top bar suggestion content during child-list collapse animation.   <-- ввёл снапшот
5a7621eef9  Added pill design to top bar suggestion.
```
Логика такая: `1c69103fc8` ввёл снапшот-заморозку и множитель `(1 - progress)` для основного
класса, а `UnconfirmedAuthWrap` тогда получил снапшот-путь, но **не** получил множитель на живом
пути и **не** получил narrow-порог. Коммит `f0dc7d4850` закрывает оба этих хвоста.

---

## 6. Оценка и риски (для брейншторма)

### Что сделано хорошо
- Фикс минимальный, точечный, без побочных изменений.
- Оба изменения — выравнивание по уже принятым конвенциям (narrow-порог + формула высоты),
  а не самодеятельность. Легко ревьюить, легко защищать.
- Граничные значения `_collapseProgress` (0 и 1) отрабатывают корректно.

### На что смотреть при ре-тесте / что можно улучшить
1. **Резкий скачок на пороге 130px.** Ниже порога — высота 0, на пиксель выше — сразу полная (с учётом
   progress). При медленном ресайсе колонки мышью около 130px возможно «моргание» появления/исчезновения.
   Соседи (`requests_bar`) прячут только текст, а не весь бар, поэтому у них перехода по высоте нет.
   Кандидат на будущее: гистерезис или увязать порог с `_collapseProgress`, чтобы уходило плавно.
2. **`resizeToWidth` не вызывается при раннем выходе.** Внутренний `wrapped()` остаётся с прежней
   разложенной шириной, пока бар скрыт (высота 0). Пока он скрыт — неважно; но если где-то отрисовка/
   геометрия опирается на «свежую» ширину `wrapped()`, держать в уме. На практике безопасно, т.к. при
   высоте 0 виджет не рисуется.
3. **Порядок проверок.** `newWidth < .../2` стоит **выше** снапшот-ветки. Значит даже во время анимации
   через снапшот в очень узкой колонке сначала сработает ранний `return 0`. Поведение консистентно
   (в узком режиме бар скрыт при любом раскладе), но стоит убедиться, что `_collapseSnapshot` при этом
   корректно освобождается позже (через `releaseCollapseSnapshot()` при `progress == 0.`), иначе пиксмап
   переживёт свою полезность. Сейчас освобождение завязано на `progress`, а не на ширину — утечки нет,
   но снапшот может «висеть» в памяти, пока колонка узкая и анимация не завершилась в 0.
4. **Тестовый сценарий:** активный алерт о новом входе + одноколоночный узкий режим + открытие
   чата/форума поверх списка (гонит `childListShown` 0→1) + параллельный ресайс окна около 130px.
   Именно на пересечении этих трёх факторов проявлялись оба исходных бага.

### Вердикт
Корректный, дисциплинированный фикс двух реальных дефектов раскладки. Изменение соответствует
паттернам кодовой базы (narrow-порог `columnMinimalWidthLeft/2` и формула `height * (1 - collapseProgress)`
из класса-брата). Риск регрессии низкий; основной остаточный нюанс — потенциальное «моргание» ровно
на пороге 130px, которое при желании можно сгладить отдельно.

---

## Приложение: ключевые ссылки на код

- Метод: `Telegram/SourceFiles/dialogs/ui/dialogs_top_bar_suggestion_content.cpp:169`
- Класс-брат (эталон формулы): там же, `:587` (`TopBarSuggestionContent::resizeGetHeight`)
- Драйвер `collapseProgress`: `dialogs/dialogs_top_bar_suggestion.cpp:79` (`setCollapseProgress(childListShown)`)
- Снапшот-заморозка: `prepareCollapseSnapshot()` / `releaseCollapseSnapshot()` там же, `:151` / `:159`
- Триггер снапшота: `dialogs/suggestions/suggestion_unreviewed_auth.cpp:131`
- Константа: `window/window.style:20` → `columnMinimalWidthLeft: 260px`
- Порог narrow по кодовой базе: `dialogs_inner_widget.cpp:865/1290/1368/1490`, `requests_bar.cpp:154`,
  `group_call_bar.cpp:257/274`, `mainwidget.cpp:2618`
