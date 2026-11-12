<div dir="rtl">

# دليل المودرز: دعم ملفات RA3 W3X في EastWind

هذا الدليل يشرح كيف يحمّل محرك EastWind ملفات `.w3x` (نموذج Red Alert 3 بصيغة XML) مباشرة، دون تحويلها إلى `.w3d`، وكيف تستعملها في ملفات INI الخاصة بـ Zero Hour. كل ما في الدليل مأخوذ من شيفرة المحرك (commits `52500ac2b` و`82546e4c2` والتعديلات غير المثبتة في `meshmdlw3x.cpp` و`W3DModelDraw.cpp`) ومن `EastWind/Shaders/mesh.hlsl`. ما لا تقوله الشيفرة لم نكتبه.

## الفهرس

1. ما هو دعم W3X وأين تضع الملفات
2. قواعد التسمية
3. استعمال INI
4. المواد والخامات
5. الـ Skinning
6. الحركات (Animations)
7. المباني
8. نقل وحدة RA3 خطوة بخطوة، وجدول استكشاف الأخطاء
9. مرجع مسجلات الـ shader (للمطورين)
10. حدود معروفة

---

## 1. ما هو دعم W3X وأين تضع الملفات

المحرك يقرأ ملف `.w3x` عبر محلل XML (مكتبة `pugixml`) ويبني منه نفس الكائنات التي يبنيها من `.w3d`: الهيكل العظمي (HTree)، الشبكات (Mesh)، صناديق الاصطدام، الحاويات (HLod)، والحركات. لا يوجد أي تحويل إلى بايتات W3D في الذاكرة.

| الشيء | الموقع |
|---|---|
| ملفات `.w3x` (النماذج، الهياكل `_SKL`، الحركات) | `Art\W3D` (نفس المجلدات التي يبحث فيها المحرك عن `.w3d`) |
| الخامات `.tga` / `.dds` | `Art\Textures` |
| `pugixml.dll` | بجانب ملف التشغيل (`.exe`) |

قواعد مهمة:

- **`.w3d` يفوز على `.w3x`.** عندما يطلب المحرك `X.w3d` ولا يجده يحاول `X.w3x`. إن وُجد الاثنان بنفس الاسم يُستعمل `.w3d` فقط.
- **لا حدود لطول الأسماء في W3X.** أسماء الهيكل والعظام والحركة وصناديق الاصطدام صارت نصوصاً بلا حجم ثابت. أما ملفات W3D فتبقى بحدودها القديمة (16 و32 بايت) دون تغيير.
- ملف W3X لا يُحمَّل إلا إذا نجح تحليله كاملاً. عند وجود خطأ في الـ XML (أو شبكة بلا رؤوس أو بلا مثلثات أو بمثلث بفهرس خاطئ) لا يُسجَّل شيء من الملف، والنتيجة كأن الملف غير موجود.
- الشبكة التي فيها أكثر من 65535 رأساً تُرفض (فهارس المثلثات 16 بت).
- رسائل التحذير (`WWDEBUG_SAY`) فارغة في نسخة Release، فلا ترى سبب الرفض إلا في نسخة Debug.

---

## 2. قواعد التسمية

### 2.1 النموذج = اسم الملف بدون الامتداد (stem)

الاسم الذي تكتبه في `Model = ...` هو اسم الملف بدون `.w3x`. مثلاً `Model = AUAntiVehicleInfantry_SKN` يحمّل الملف `AUAntiVehicleInfantry_SKN.w3x`. المقارنة لا تفرّق بين الأحرف الكبيرة والصغيرة.

### 2.2 قاعدة الـ stem

اسم الملف (stem) هو الذي يسمّي ما بداخله:

- **الحاوية (container)** المسجّلة باسم الـ stem.
- **الشبكات والصناديق** باسم `STEM.SUB` حيث `SUB` هو الجزء بعد آخر نقطة في `id` الشبكة. مثلاً الشبكة `id="AUGUNSHIP_SKN.NEWSKIN"` في الملف `AUGunship_SKN.w3x` تسمّى `AUGunship_SKN.NEWSKIN`.
- أي هيكل أو حركة محلية يساوي `id` الخاص بها `id` الحاوية تُسجَّل باسم الـ stem.

فائدة القاعدة: ملفات مثل `ABAirfield_L.w3x` تسجّل `ABAirfield_L` ولا تتصادم مع `ABAirfield`.

عند التكرار (اسم مسجّل مسبقاً): **الأول يفوز** والثاني يُتجاهل.

### 2.3 أسماء الشبكات الفرعية (Sub-objects)

في `HideSubObject` و`ShowSubObject` تكتب الجزء الأخير فقط (`SUB`)، فالمحرك يقارن الاسم الكامل أولاً ثم الجزء بعد أول نقطة، دون حساسية لحالة الأحرف. للعثور على الأسماء افتح ملف الـ SKN وابحث عن `<W3DMesh id="...">`.

### 2.4 الحركة = `HIERARCHY.ANIM`

الحركة تُسمّى في INI بالشكل:

```
Animation = <معرّف الهيكل>.<معرّف الحركة>
```

مثال: `AUANTIVEHICLEINFANTRY_SKL.AUANTIVEHICLEINFANTRY_AIDA`.

- الجزء الأول هو `id` الـ `W3DHierarchy` (ملف `_SKL`).
- المحرك يحمّل الحركة من ملف اسمه **الجزء بعد النقطة**: أي `AUANTIVEHICLEINFANTRY_AIDA.w3x`. لذلك يجب أن يطابق اسم ملف الحركة معرّفها.
- الحركة نفسها تعلن `Hierarchy="..."` وتستورد الهيكل عبر `Includes` (انظر 2.5).

### 2.5 الحاويات والهياكل و`Includes`

عناصر الملف التي يقرؤها المحلل: `W3DHierarchy` و`W3DMesh` و`W3DCollisionBox` و`W3DContainer` و`W3DAnimation` و`Includes`. أي عنصر آخر يُتجاهل مع تحذير.

```xml
<AssetDeclaration>
  <Includes>
    <Include source="ART:AUAntiVehicleInfantry_SKL.w3x"/>
    <Include source="ART:AUAntiVehicleInfantry.xml"/>
  </Includes>
  ...
</AssetDeclaration>
```

- `Include` بامتداد `.w3x` = ملف هيكل عظمي. يُحمَّل عبر النظام العادي، فإن وُجد `.w3d` بنفس الاسم فهو الذي يفوز.
- `Include` بامتداد آخر (`.xml`) = ملف تعريف خامات (`<Texture id="..." File="..."/>`). تعريفات الملف نفسه تفوز على تعريفات الـ includes.
- الحاوية `W3DContainer` تضم `SubObject` كل منها يشير إلى شبكة أو صندوق ورقم عظمة (`BoneIndex`). رقم عظمة خارج الهيكل يُحذف مع تحذير.
- الشبكة من نوع `Skin` تحتاج هيكلاً بصيغة **W3X** (لأن وضعية الربط تُقرأ منه). إن كان الهيكل `.w3d` تُتخطّى الشبكة.
- سلسلة includes تعود إلى نفس الملف تتوقف تلقائياً.

---

## 3. استعمال INI

الوحدات تُعرَّف في `Draw = W3DModelDraw` كالمعتاد. مثال حقيقي من حزمة الاختبار (Javelin = `AmericaInfantryMissileDefender`):

```ini
Object AmericaInfantryMissileDefender
  Scale                  = 0.75

  Draw = W3DModelDraw ModuleTag_01
    OkToChangeModelColor = Yes

    DefaultConditionState
      Model             = AUAntiVehicleInfantry_SKN
      IdleAnimation     = AUANTIVEHICLEINFANTRY_SKL.AUANTIVEHICLEINFANTRY_AIDA 0 30
      IdleAnimation     = AUANTIVEHICLEINFANTRY_SKL.AUANTIVEHICLEINFANTRY_AIDB
      IdleAnimation     = AUANTIVEHICLEINFANTRY_SKL.AUANTIVEHICLEINFANTRY_AIDE
      AnimationMode     = ONCE
      AnimationSpeedFactorRange = 0.8 1.2
      TransitionKey     = TRANS_Stand
      WeaponFireFXBone  = PRIMARY ROCKET
      WeaponLaunchBone  = PRIMARY ROCKET
      WeaponLaunchBone  = SECONDARY ROCKET
      HideSubObject     = FX_LASER
      ShowSubObject     = ROCKET
    End

    ConditionState      = FIRING_A
      Animation         = AUANTIVEHICLEINFANTRY_SKL.AUANTIVEHICLEINFANTRY_ATKA
      AnimationMode     = ONCE
      HideSubObject     = ROCKET
    End

    ConditionState      = MOVING
      Animation         = AUANTIVEHICLEINFANTRY_SKL.AUANTIVEHICLEINFANTRY_RUNA 20
      AnimationMode     = LOOP
      Flags             = RANDOMSTART
    End
  End
End
```

### 3.1 المفاتيح

| المفتاح | المعنى |
|---|---|
| `Model` | اسم ملف الـ SKN بدون امتداد (انظر 2.1). `NONE` = لا نموذج. |
| `Animation` | حركة واحدة بالشكل `HIERARCHY.ANIM`. بعد الاسم اختيارياً: المسافة المقطوعة ثم عدد التكرار. |
| `IdleAnimation` | حركات الخمول (يُختار بينها). لا تضع مسافة مقطوعة غير صفرية لها. |
| `AnimationMode` | `MANUAL` أو `LOOP` أو `ONCE` أو `LOOP_PINGPONG` أو `LOOP_BACKWARDS` أو `ONCE_BACKWARDS`. |
| `HideSubObject` / `ShowSubObject` | أسماء شبكات فرعية (الجزء الأخير). الإخفاء يشمل أيضاً الشبكات الأخرى المعلّقة على نفس العظمة. |
| `Flags` | `RANDOMSTART` و`START_FRAME_FIRST` و`START_FRAME_LAST` و`ADJUST_HEIGHT_BY_CONSTRUCTION_PERCENT` و`PRISTINE_BONE_POS_IN_FINAL_FRAME` و`MAINTAIN_FRAME_ACROSS_STATES` (حتى 4) و`RESTART_ANIM_WHEN_COMPLETE` و**`ANIMATE_BY_CONSTRUCTION_PERCENT`** (جديد، القسم 7). |
| `Scale` | تكبير/تصغير النموذج (في كتلة `Object`). |

### 3.2 العظام: `NAME` مقابل `NAME01`

مفاتيح مثل `WeaponFireFXBone` و`WeaponLaunchBone` و`WeaponRecoilBone` و`WeaponMuzzleFlash` و`Bone:` في أنظمة الجسيمات تبحث بهذه القاعدة (تحويل الاسم إلى أحرف صغيرة أولاً):

1. تبحث عن عظمة اسمها `NAME` تماماً.
2. ثم `NAME01` ثم `NAME02` ... حتى `NAME99`، وتتوقف عند أول رقم غير موجود.
3. إن لم تجد أي عظمة بهذا الاسم تبحث عن **شبكة فرعية** بنفس الاسم وتستعمل مكانها.

أي أن `WeaponFireFXBone = PRIMARY FXGUN_FL` تعمل سواء كانت العظمة `FXGUN_FL` أو `FXGUN_FL01` و`FXGUN_FL02`. أما المفاتيح التي تأخذ اسم عظمة واحدة مباشرة (مثل `AttachParticleBone` و`BladeBoneName` في حزمة الاختبار) فاكتب الاسم كاملاً كما هو في `<Pivot Name="...">`.

للعثور على أسماء العظام: افتح ملف `_SKL.w3x` وابحث عن `<Pivot Name="...">`. أسماء RA3 تختلف عن أسماء Zero Hour (مثلاً `Muzzle` صارت `ROCKET`، و`Smoke` صارت `FXDAMAGE`)، فعدّل كل أسماء العظام في الكتلة.

### 3.3 `Scale` و`Geometry`: القاعدة المهمة

- `Scale` يكبّر **الرسم** فقط.
- الـ Geometry (`GeometryMajorRadius` و`GeometryMinorRadius` و`GeometryHeight`) **لا يتأثر بـ `Scale`**. هذه هي صندوق الإصابة والتصادم، وتحدده أنت في INI.

لذلك:

```
GeometryMajorRadius = (نصف العرض على X من BoundingBox) × Scale
GeometryMinorRadius = (نصف العرض على Y من BoundingBox) × Scale
GeometryHeight      = (أعلى Z من BoundingBox) × Scale
```

القيم `Min`/`Max` تؤخذ من عنصر `<BoundingBox>` للشبكة الرئيسية داخل ملف SKN.

**مثال رقمي (أرقام افتراضية للتوضيح):** شبكة عرضها على X من `-36.5` إلى `+36.5`، وعلى Y من `-20` إلى `+20`، وأعلى نقطة Z = `15`، و`Scale = 0.55`:

```
GeometryMajorRadius = (36.5 - (-36.5)) / 2 × 0.55 = 36.5 × 0.55 = 20.075
GeometryMinorRadius = (20 - (-20))     / 2 × 0.55 = 20   × 0.55 = 11.0
GeometryHeight      = 15 × 0.55 = 8.25
```

إن نسيت الضرب في `Scale` وكتبت `36.5` فصندوق الإصابة سيكون أكبر من الوحدة المرسومة بنحو الضعف.

---

## 4. المواد والخامات

المحرك يقرأ **ثوابت** كتلة `FXShader` في كل شبكة بأسمائها، ولا يهمّه اسم ملف الـ `.fx` إلا في حالة واحدة: ألوان المادة والـ `VertexColors` تُقرأ فقط عندما يكون `ShaderName` هو `DefaultW3D.fx`.

أغلب المزايا التالية تحتاج في `Options.ini`:

```ini
PerPixelLighting = yes
NormalMaps       = yes
```

(الإضاءة لكل بكسل شرط لخرائط المواد؛ بدونها يعمل المسار الكلاسيكي ويتجاهل `_NRM` و`_SPM` والـ house colour الخاص بالـ shader وخريطة الضرر.)

### 4.1 جدول الثوابت المدعومة

| الثابت (اسمه في XML) | الأثر |
|---|---|
| `DiffuseTexture` أو `Texture_0` | خامة المرحلة 0. المعرّف `id` يُحوَّل إلى ملف: إن عُرِّف في `.xml` (`<Texture id File>`) يُستعمل اسم ملفه، وإلا `<id>.tga`. يُجرَّب `.dds` قبل `.tga`. تُستعمل أسماء الملفات فقط (بلا مسار). |
| `NormalMap` | لا يُحمَّل بالاسم. المحرك يبحث عن `<diffuse>_NRM` بجانب الخامة. إن كان الاسم المعلن مختلفاً يُسجَّل تحذير ولن تُوجد الخريطة. |
| `SpecMap` | نفس الشيء: `<diffuse>_SPM`. |
| `AlphaTestEnable` | اختبار ألفا. |
| `CullingEnable = false` | عرض وجهين (two-sided). |
| `DepthWriteEnable` | كتابة العمق. |
| `BlendMode` | `0` معتم، `1` ألفا (blend)، `2` جمعي (additive). أي قيمة أخرى تُرسم معتمة مع تحذير. |
| `AlphaBlendingEnable = true` | يُعامل كـ `BlendMode = 1`. |
| `SortLevel` (خاصية الشبكة) | يُقصّ إلى الحد الأقصى للمحرك. الشبكة الممزوجة (غير alpha-test) بلا `SortLevel` تُفرز تلقائياً. |
| `Hidden` و`CastShadow` و`GeometryType` | `Skin` و`CameraAligned` و`CameraOriented` تُحوَّل لأعلام الشبكة. نوع غير معروف = رفض الملف كله. |
| `ColorAmbient` / `ColorDiffuse` / `ColorSpecular` / `ColorEmissive` / `Shininess` / `Opacity` | **فقط** مع `DefaultW3D.fx`. |
| `VertexColors` | **فقط** مع `DefaultW3D.fx` وبعدد مطابق للرؤوس. |
| `NumTextures >= 2` + `Texture_1` + مجموعة UV ثانية | خامة ثانية (4.4). |
| `SecondaryTextureBlendMode` | `0` ضرب، `1` ضرب ×2، `2` (و`3`) مزج تفصيلي (detail blend). |
| `TexCoordMapper_N` + `TexCoordTransform_N` | إزاحة UV متحركة (4.5). |
| `TextureAnimation_FPS_NumPerRow_LastFrame_FrameOffset_0` | فليب بوك (4.5). |
| `EdgeFadeOut` | تلاشي عند الحواف (4.3). |
| `BumpScale` | قوة الخريطة العادية (4.3). |
| `EmissiveHDRMultipler` (بهذا الإملاء) | مضاعف الإضاءة الذاتية (4.3). |
| `DamagedTexture` | خامة الضرر (4.7). |

أي ثابت خامة آخر (غير ما سبق) لا يُحوَّل، ويُسجَّل تحذير "is not converted".

### 4.2 خرائط المواد: `_NRM` و`_SPM`

بجانب الخامة الأساسية `NAME.tga` يبحث المحرك (مرة واحدة عند أول رسم) عن:

- `NAME_nrm` (`.dds` أو `.tga`): خريطة عادية بنظام DirectX (الأخضر = +v، لأسفل الصورة). المحرك يحسب إطار المماس من مشتقات الشاشة، فلا حاجة لبيانات tangent في الشبكة.
- `NAME_spm`: خريطة SpecMap، قنواتها: **R** قوة اللمعان، **G** التوهج (إضاءة ذاتية: تُظهر الخامة بلا إضاءة)، **B** قناع لون اللاعب (house colour).

الشرط: الشبكة تستعمل المرحلة 0 فقط. شبكة فيها `Texture_1` (المرحلة 1 مفعّلة) تُرسم بدون خرائط المواد.

### 4.3 EdgeFadeOut وBumpScale وHDR والمزج

- **EdgeFadeOut**: قيمة `e` أكبر من صفر. المعامل `smoothstep(saturate(abs(dot(N,V)) / e))`. في المزج الجمعي (additive) يضرب **اللون**، وفي غيره يضرب **ألفا**. يعمل في مسار الإضاءة لكل بكسل فقط (`MeshPS` / `MeshMaterialPS`)، لا في `UnlitPS`.
- **BumpScale**: يضرب مركّبتي xy للخريطة العادية قبل التحويل. يعمل فقط حين تُستعمل خرائط المواد (وجود `_NRM` أو `_SPM`). الافتراضي 1.
- **EmissiveHDRMultipler**: يُقصّ بين 1 و4 ثم يضرب ألوان emissive في المادة. حين يزيد عن 1 يُفتح قصّ اللون عند 1 لهذه الشبكة، فتبقى الإضاءة فوق الأبيض وتصلح مع `Hdr = yes` و`Bloom = yes`. تذكّر أن اللون الأصلي للـ emissive يأتي فقط من `ColorEmissive` في `DefaultW3D.fx`.
- **الدمج:** `BlendMode = 2` جمعي، `1` ألفا (انظر 4.1). الشبكة الممزوجة بلا alpha test تُفرز تلقائياً.

### 4.4 خامتان: `Texture_1` + `SecondaryTextureBlendMode`

الشروط معاً: خامة أساسية موجودة، `NumTextures >= 2`، ثابت `Texture_1` نصي غير فارغ، **وملف الخامة الثانية موجود**، والشبكة تحتوي مجموعة `TexCoords` ثانية بنفس عدد الرؤوس. إن فُقد أي شرط تُسقط الخامة الثانية مع تحذير. المزج يُضبط من `SecondaryTextureBlendMode` (0 ضرب، 1 ضرب ×2، 2 أو 3 مزج تفصيلي).

### 4.5 إزاحة UV والـ Flipbook

يُنشأ الـ mapper من الثوابت (للمرحلة 0، والمرحلة 1 إزاحة فقط):

**إزاحة/تكبير UV** (يلزم `TexCoordMapper_N = 1`):

```
TexCoordTransform_N = scaleU  scaleV  scrollU  scrollV
```

- إن كان التمرير `0 0` وكان التكبير `1 1` فلا mapper.
- إن كان التمرير `0 0` والتكبير غير `1 1`: تكبير فقط.
- غير ذلك: تمرير بمقدار `scrollU, scrollV` في الثانية (الإشارة كما في RA3).

**Flipbook** (المرحلة 0 فقط):

```
TextureAnimation_FPS_NumPerRow_LastFrame_FrameOffset_0 = FPS  NumPerRow  LastFrame  FrameOffset
```

- يُفعَّل إذا كان `NumPerRow >= 2` وكان `FPS != 0` **أو** `TexCoordMapper_0 = 2`. القيمة الافتراضية "بلا حركة" هي `0 1 1 0`.
- الشبكة مربعة `NumPerRow × NumPerRow`، والخلية رقم `f` تقع في العمود `f % NumPerRow` والصف `f / NumPerRow`، وحجم الخلية `1 / NumPerRow`. **لا يشترط أن يكون `NumPerRow` من قوى 2** (مثلاً 3 أو 5 تعمل).
- `LastFrame = 0` تعني `NumPerRow²`. `FrameOffset` يُلفّ بباقي القسمة على `LastFrame`.
- يجب أن تغطي UV الشبكة خلية واحدة فقط، فالمحرك يضيف الإزاحة.

مثال: شبكة 3×3 بسرعة 2 إطار/ثانية، 9 إطارات:

```xml
<Float Name="TextureAnimation_FPS_NumPerRow_LastFrame_FrameOffset_0">
  <Value>2</Value><Value>3</Value><Value>9</Value><Value>0</Value>
</Float>
```

### 4.6 لون اللاعب (House colour) وقناع HC_

يعمل مع خرائط المواد (`PerPixelLighting` و`NormalMaps` مفعّلتان). يستعمل المحرك لون اللاعب للكائن، والمزج من نوع overlay: الرمادي المتوسط يعطي اللون نفسه، والأغمق يغمّق، والأفتح يفتّح بدل أن يقصّ.

مصدر القناع بحسب الملفات الموجودة بجانب الخامة `NAME`:

1. إن وُجد `NAME_SPM`: القناع = قناته **B**. (مباني، مركبات.)
2. وإلا إن وُجد **`HC_NAME`** (البادئة قبل اسم الملف): القناع = قناة **ألفا** منه، ولا يوجد لمعان ولا توهج. (مشاة RA3، مثل `HC_AUAntiVehicleInfantry`.)

ملاحظات:

- خامة اسمها يبدأ بـ `#` تُعتبر ملوّنة من اللعبة على المعالج، فلا يُطبَّق عليها القناع.
- وحدات حزمة الاختبار تضع `OkToChangeModelColor = Yes` (كما في مثال القسم 3).

### 4.7 خامة الضرر `DamagedTexture`

- تُركَّب على المرحلة 1 ويشترط أن يكون في الشبكة مجموعة UV ثانية بنفس عدد الرؤوس، وألا تكون هناك `Texture_1`.
- **تُركَّب فقط إذا كان ملفها موجوداً.** وإلا لا تظهر (لا نموذج "خامة مفقودة").
- تعمل مع مسار خرائط المواد، أي يلزم أن يكون للخامة الأساسية `_NRM` أو `_SPM` (أو `HC_`) و`NormalMaps = yes`.
- المحرك يضبط مقدار الضرر لكل رسم من حالات النموذج:

| حالة الكائن | المقدار |
|---|---|
| سليم | 0 |
| `DAMAGED` | 0.5 |
| `REALLY_DAMAGED` أو `RUBBLE` | 1.0 |

الـ shader يضرب الخامة الأساسية بـ `lerp(1, خامة_الضرر, المقدار)`، فتظهر الخامة تدريجياً، وألفا الضرر يمكن أن يثقب الشبكة عند تفعيل alpha test. قناع الضرر لكل رأس في RA3 (ألفا الرؤوس) لا يُستعمل في المحرك.

### 4.8 الخامة المفقودة

التعديل الحالي (غير مثبت بعد في commit) يمنع ظهور خامة "المفقود" الوردية/الرمادية:

| الحالة | ما يحدث |
|---|---|
| خامة diffuse غير موجودة | تُسقط، ويُعطَّل التركيب (texturing) فيظهر لون المادة فقط. وإن كانت الشبكة ممزوجة أو `SortLevel > 0` تُخفى (`Hidden`) كي لا تظهر كتلة صلبة، مع بقاء اسمها قابلاً لـ `HideSubObject`/`ShowSubObject`. |
| `Texture_1` غير موجودة | تُسقط الخامة الثانية فقط. |
| `DamagedTexture` غير موجودة | لا تُركَّب. |
| `_NRM` / `_SPM` / `HC_` غير موجودة | لا تُحمَّل (المحرك يتحقق من وجود الملف أولاً)، فلا خرائط مواد. |

---

## 5. الـ Skinning

- **Rigid:** كل رأس يتبع عظمة واحدة. أغلب نماذج RA3 هكذا.
- **عظمتان (two-bone blended):** الرأس يحتفظ بعظمته الأولى (الأثقل وزناً) وبعظمة ثانية ووزنها، والنتيجة مزج الموضعين والعمودين. يُقرأ من كتلتَي `Vertices`/`BoneInfluences` في الشبكة (الحد الأقصى كتلتان). إن لم يكن في الشبكة أي رأس ممزوج فهي rigid.

### مسار GPU

يخطّ الرؤوس في vertex shader. في `Options.ini`:

| المفتاح | الافتراضي | الأثر |
|---|---|---|
| `GpuSkinning` | `yes` | تشغيل الـ skinning على الـ GPU. |
| `PerPixelLighting` | `no` (افتراضي الشيفرة) | **شرط للـ GPU skinning**، لأن الشبكات المعبّأة تُرسم عبر shaders الإضاءة. |
| `DebugKeys` | `no` | عند `yes`: `Ctrl+Shift+F11` يبدّل إضاءة الوحدات (والـ GPU skin معها)، و`Ctrl+Shift+F12` خرائط المواد، و`Ctrl+Shift+T` يعرض أزمنة GPU. |

للشبكة الممزوجة: رأس 80 بايت (موضع وعمود وصف palette وعامل وزن للعظمة الثانية)، و`MeshSkin2VS`، وshaders ظل خاصة (casters). الـ palette تجمع عظام الاثنتين معاً.

### الرجوع إلى المعالج (CPU)

يُستعمل مسار CPU (ويحسب المزج أيضاً) في هذه الحالات (من قائمة الأسباب في المحرك):

- `GpuSkinning = no` أو `PerPixelLighting = no` أو غياب `vs_3_0`.
- الشبكة تُرسم عبر الـ sorting renderer (ممزوجة/مرتبة).
- للشبكة تمريرات مواد (material passes).
- أكثر من **64 عظمة مختلفة** (`MESH_MAX_BONES`).
- لا HTree، أو فشل بناء الـ vertex buffer، أو رسم لا يقبله `MeshShader` (clip planes وما شابه).

لا يغيّر مسار CPU الشكل النهائي، فقط الأداء.

---

## 6. الحركات (Animations)

### 6.1 قنوات الحركة المدعومة

| القناة (`Type`) | النوع |
|---|---|
| `Orientation` | `ChannelQuaternion` |
| `XTranslation` / `YTranslation` / `ZTranslation` | `ChannelScalar` |
| `Visibility` | `ChannelScalar` |

أي قناة أخرى تُتجاهل. `FrameRate` الافتراضي 30، والإطارات حتى 65535.

### 6.2 مفاتيح الخطوة (BinaryMove)

كل `<Frame>` يمكن أن يحمل `BinaryMove="true"`. إذا كان الإطار التالي في العينة علامة خطوة، **يثبت المحرك قيمة الإطار السابق ولا يمزج**، فتقفز القيمة عند الإطار الموسوم. يُطبَّق لكل قناة (X وY وZ منفصلة) وللدوران.

### 6.3 القنوات الجزئية والرؤية

- قناة لا تغطي كل الإطارات (تبدأ بعد `0` أو تنتهي قبل النهاية) تُمدّ إلى `[0 .. آخر إطار]` **بتثبيت أول مفتاح وآخر مفتاح**. ليست "هوية خارج المدى".
- قناة `Visibility`: تتحول إلى قناة bits، القيمة غير الصفرية = ظاهر (RA3 تكتب true كـ `-1`)، وخارج المدى الشيء ظاهر.

### 6.4 مضغوط وغير مضغوط

الحركات في W3X تُحمَّل دائماً كحركات خام (`HRawAnimClass`) بإطار لكل قيمة. عنصر `CompressionSettings` الموجود في بعض الحركات يُهمَل (معلومات من المصدِّر فقط). لا يُنشأ مسار الحركات المضغوطة (`HCompressedAnimClass`) من W3X، وكل الإطارات تُقرأ كما هي.

### 6.5 كيف تجد معرّفات الحركات

1. اعرف ملفات الحركة: بجانب `X_SKN.w3x` و`X_SKL.w3x` ملفات مثل `X_AIDA.w3x` و`X_ATKA.w3x` و`X_RUNA.w3x`. (في حزمة Javelin: `AIDA` `AIDB` `AIDE` `ATKA` `RUNA` `RLDA` `DTBA` `DTBB` `DTKA` `FLYA` `LNDA`).
2. افتح ملف الحركة وانظر `<W3DAnimation id="..." Hierarchy="...">`. المعرّف هو الجزء بعد النقطة، والهيكل هو الجزء قبلها.
3. اسم الملف يجب أن يساوي المعرّف (انظر 2.4).
4. تأكد أن الهيكل هو نفسه المستعمل في الـ SKN.

---

## 7. المباني

### 7.1 نمط البناء `ANIMATE_BY_CONSTRUCTION_PERCENT`

الفلاج الجديد في `Flags` يجعل الحركة تتبع **نسبة البناء** بدل الساعة. في كل إطار رسم، إن كانت الحالة الحالية تحمله والنموذج حاوية HLod والنسبة `>= 0`، يضبط المحرك الحركة على الإطار `(عدد الإطارات − 1) × نسبة/100` بوضع `MANUAL`. النسبة تُقصّ بين 0 و100. عند اكتمال البناء (`-1`) لا يتدخل الفلاج.

يتطلب: `AnimationMode = MANUAL` وحركة فيها أكثر من إطار واحد.

النمط الكامل (من حزمة الاختبار على `AmericaAirfield`):

```ini
Object AmericaAirfield
  Scale = 0.75

  ; 1) الوحدة الأساسية: في حالات البناء لا نموذج
  Draw = W3DModelDraw ModuleTag_01
    DefaultConditionState
      Model = ABAirfield
    End

    ConditionState = AWAITING_CONSTRUCTION PARTIALLY_CONSTRUCTED ACTIVELY_BEING_CONSTRUCTED
      Model = NONE
    End
    ConditionState = AWAITING_CONSTRUCTION
      Model = NONE
    End
    AliasConditionState = SOLD
  End

  ; 2) وحدة رسم إضافية تظهر أثناء البناء فقط
  Draw = W3DModelDraw ModuleTag_EWBuildUp
    DefaultConditionState
      Model = NONE
    End
    ConditionState = PARTIALLY_CONSTRUCTED
      Model         = ABAirfield_BLD_SKN
      Animation     = ABAIRFIELD_BLD_SKL.ABAIRFIELD_BLD_BLD
      AnimationMode = MANUAL
      Flags         = ANIMATE_BY_CONSTRUCTION_PERCENT
    End
    AliasConditionState = PARTIALLY_CONSTRUCTED ACTIVELY_BEING_CONSTRUCTED
  End
End
```

(المقطع الأول مختصر؛ في الملف الفعلي كل تركيبات `DAMAGED` و`REALLYDAMAGED` و`NIGHT` و`SNOW` تحتاج `Model = NONE` أيضاً.)

### 7.2 حالات البناء = `Model = NONE`

في الوحدة الأساسية اجعل كل حالات `AWAITING_CONSTRUCTION` و`PARTIALLY_CONSTRUCTED` و`ACTIVELY_BEING_CONSTRUCTED` (بكل تركيباتها مع الضرر والليل والثلج) و`SOLD` على `Model = NONE`، ودع وحدة الـ build-up وحدها ترسم أثناء البناء. وإلا ستُرسم النسختان معاً.

### 7.3 البيع

عند البيع يضبط المحرك النسبة على 99.9 ويضع الحالتين `PARTIALLY_CONSTRUCTED` و`ACTIVELY_BEING_CONSTRUCTED`، ثم بعد فترة سماح قصيرة يُنقص النسبة خلال نحو **3 ثوان** حتى الصفر. بما أن الحركة تتبع النسبة، **تُعرض حركة البناء بالعكس تلقائياً** عند البيع. عند وصول النسبة للصفر تُمسح الحالتان وتوضع `SOLD`.

### 7.4 ملاحظة عن العظام ذات الأسماء الثابتة

بعض سلوكيات اللعبة تبحث عن عظام بأسماء ثابتة في الشيفرة (مثل `ParkingPlaceBehavior` الذي يبني `Runway1Parking1` وأمثالها). أسماء RA3 مختلفة، فحزمة الاختبار أضافت/أعادت تسمية عظام في نسخة من `ABAirfield.w3x`. عند نقل مبنى لسلوك يعتمد أسماء عظام ثابتة افتح السلوك وتأكد من الأسماء.

---

## 8. نقل وحدة RA3 خطوة بخطوة (مثال Javelin)

الهدف: استعمال جندي Javelin (`AUAntiVehicleInfantry`) كشكل لـ `AmericaInfantryMissileDefender`.

**الخطوة 1: انسخ الملفات.**

```
Art\W3D\AUAntiVehicleInfantry_SKN.w3x
Art\W3D\AUAntiVehicleInfantry_SKL.w3x
Art\W3D\AUAntiVehicleInfantry_AIDA.w3x   (وبقية الحركات: AIDB AIDE ATKA RUNA RLDA DTBA DTBB DTKA FLYA LNDA)
Art\Textures\  <الخامة الأساسية>.tga  و  _NRM  و  _SPM  و  HC_<الخامة>.tga
```

اسم الخامة الأساسية تقرؤه من ثابت `DiffuseTexture` / `Texture_0` داخل ملف الـ SKN. القناع `HC_` للمشاة بجانب الخامة بالبادئة `HC_`.

**الخطوة 2: اجمع الأسماء من الملفات.**

- شبكات فرعية: `<W3DMesh id="…">` في الـ SKN (للـ `HideSubObject`).
- عظام: `<Pivot Name="…">` في الـ SKL (مثلاً `ROCKET`).
- حركات: `<W3DAnimation id="…" Hierarchy="…">` (القسم 6.5).

**الخطوة 3: عدّل INI.** استبدل `Model` وكل `Animation`/`IdleAnimation` بصيغة `HIERARCHY.ANIM` بحروف RA3، وبدّل أسماء العظام (`Muzzle` ← `ROCKET`)، وأخفِ ما لا يلزم بـ `HideSubObject` (مثل `FX_LASER`). استخدم المثال في القسم 3 وأضف `DTKA` لحالة `DYING`.

**الخطوة 4: اضبط الحجم والـ Geometry.** `Scale` من عندك، ثم الـ Geometry من `BoundingBox` مضروباً في `Scale` (القسم 3.3). مثال المركبة Gunship (`AUGunship_SKN`): `Scale = 0.55`، والنموذج عرضه نحو 73 وحدة أي نصفه 36.5، فالـ `GeometryMajorRadius` ≈ 36.5 × 0.55 ≈ 20. شبكة الجلد هناك `AUGUNSHIP_SKN.NEWSKIN`.

**الخطوة 5: اختبر.** شغّل اللعبة بنسخة Debug إن أردت قراءة التحذيرات، وفعّل في `Options.ini`: `PerPixelLighting = yes` و`NormalMaps = yes` و`DebugKeys = yes` كي تقارن `Ctrl+Shift+F12` (خرائط المواد) أثناء اللعب.

### جدول استكشاف الأخطاء

| العَرَض | السبب المحتمل | الحل |
|---|---|---|
| خامة وردية/رمادية "مفقودة" | ملف الخامة غير موجود، أو الاسم في `.xml` يشير لملف آخر، أو الخامة ليست بصيغة TGA/DDS | ضع الملف في `Art\Textures` بالاسم الصحيح. مع التعديل الحالي تُسقط الخامة المفقودة وترى لون المادة فقط. |
| الخامة تظهر لكن بلا ملمس/لمعان/لون لاعب | `_NRM` / `_SPM` / `HC_` غير موجودة أو باسم مختلف، أو الخيارات `PerPixelLighting` / `NormalMaps` مطفأة، أو للشبكة `Texture_1` | سمِّها `<diffuse>_NRM` و`<diffuse>_SPM` و`HC_<diffuse>`، وفعّل الخيارين. |
| الشبكة غير مرئية | ملف الـ SKN لم يُحمَّل (خطأ XML)، أو شبكة `Skin` مع هيكل `.w3d`، أو أكثر من 65535 رأساً، أو الشبكة `Hidden`/مخفية بـ `HideSubObject`، أو خامتها المفقودة كانت ممزوجة | تأكد أن الهيكل `_SKL` من نوع W3X، وراجع تحذيرات Debug وأسماء `HideSubObject`. |
| الحجم خطأ / صندوق الإصابة لا يطابق الرسم | نسيت أن الـ Geometry لا يتأثر بـ `Scale` | اضرب نصف الـ BoundingBox في `Scale` (القسم 3.3). |
| لا حركة خمول | لا يوجد `IdleAnimation` أو `Animation` في `DefaultConditionState`، أو اسم الحركة خطأ، أو `AnimationMode` غير مناسب | أضف `Animation = HIER.ANIM` مع `AnimationMode = LOOP`، أو `IdleAnimation` وتحقق من القسم 2.4. |
| الوحدة في وضع T-pose أو تقف متجمدة | الحركة لم تُعثر عليها (ملف الحركة غير موجود أو اسمه لا يطابق معرّفها)، أو الهيكل في الحركة غير الهيكل في الـ SKN | اسم ملف الحركة = معرّفها، و`Hierarchy` في الحركة = هيكل الـ SKL. |
| العظمة خطأ (الصاروخ/النار من مكان غير صحيح) | اسم عظمة ما زال من Zero Hour، أو `NAME` مقابل `NAME01` | استخدم أسماء الـ SKL من RA3 (القسم 3.2). |
| جزء يختفي أو يظهر فجأة أثناء حركة | قناة `Visibility` (مثل الصاروخ في `ATKA`/`RLDA`) وهذا سلوك RA3 | اضبط `HideSubObject`/`ShowSubObject` وحالات الحركة بما يوافقه. |
| حواف شفافة غير متوقعة أو توهج كبير | `EdgeFadeOut` أو `EmissiveHDRMultipler` | هذا مقصود في شبكات FX؛ قلّل القيمة في الملف. |

---

## 9. مرجع مسجلات الـ shader للمطورين: `c40` إلى `c43`

ثوابت البكسل لـ `mesh.hlsl` (الأرقام في `EastWind/Shaders/shader_constants.h` وتُضبط في `meshshader.cpp`). `MESH_PS_COUNT = 44`.

| المسجل | الاسم | x | y | z | w |
|---|---|---|---|---|---|
| `c40` | `MESH_PS_MATERIAL_OPTIONS` | خريطة عادية مفعّلة | SpecMap مفعّلة | house colour مفعّل | مقياس ambient من الأرض (0.5) |
| `c41` | `MESH_PS_HOUSECOLOR` | rgb لون اللاعب | (تابع rgb) | (تابع rgb) | قوة اللمعان لـ SpecMap (`Power` إن كانت > 1، وإلا 32) |
| `c42` | `MESH_PS_MATERIAL_EXTRA` | مقدار الضرر | خريطة الضرر مفعّلة | ألفا SpecMap هي قناع الـ house (HC_) | احتياطي |
| `c43` | `MESH_PS_MATERIAL2` | `BumpScale` (الافتراضي 1) | `EdgeFadeOut` (0 = معطّل) | 1 = الـ fade يضرب اللون (additive)، 0 = يضرب ألفا | 1 = HDR emissive (بلا قصّ عند 1) |

ملاحظات:

- `c42` يعاد استعماله في `ParticlePS` بمعنى مختلف (`PARTICLE_PS_SOFT`)، وهما shaderان مختلفان.
- `c43.z` يُضبط 1 عندما `D3DRS_DESTBLEND == D3DBLEND_ONE`.
- `c43.w` يفتح `Mesh_Clamp` (قصّ اللون عند 1) عندما `EmissiveHDR > 1`.
- قيم `c43` الأخرى تُكتب فقط عندما تكون إضافات المادة غير افتراضية؛ وإلا `BumpScale = 1` ويبقى الباقي صفراً.
- `c40` و`c42` يُكتبان فقط حين تُستعمل خرائط المواد؛ `c43` يُكتب لكل رسم مضاء.

---

## 10. حدود معروفة

- كل ما في الدليل خاص بمسار Direct3D 9 (`EastWind`)، ومعظم مزايا المواد تتطلب `PerPixelLighting = yes` و`NormalMaps = yes`.
- قناع الضرر لكل رأس في RA3 لا يُستعمل؛ الضرر يُحدَّد لكامل الكائن من حالته.
- الـ Flipbook وأي mapper متحرك لا يُعرَّفان إلا على المرحلة 0 (الإزاحة تدعم المرحلة 1 أيضاً).
- غير مقروء في الشيفرة: الـ tangents المحفوظة و`AABTree` و`FixupMatrix` وثوابت مثل `HouseColorPulse` و`UseRecolorColors` و`UseWorldCords`.
- إن كان في المجلدات `.w3d` و`.w3x` بنفس الاسم فسيُستعمل `.w3d` فقط (قد يبدو الأمر "تعديلاً لا يظهر").

</div>
