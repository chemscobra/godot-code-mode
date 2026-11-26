# Design

## Context

Véase `proposal.md` para la motivación y el alcance. El cambio es transversal: afecta al modelo de metadatos, la conversión en ambos sentidos, la resolución de referencias y la respuesta de ejecución, por lo que requiere diseño explícito.

Evidencia de 0.2.1:
- `src/js_worker.cpp` transforma etiquetas antes de conocer el destino y usa un paso JSON que puede perder valores o ejecutar `toJSON`.
- `src/bridge.cpp` representa Vector2, Vector3 y Color como datos y otros valores no JSON como referencias. La asignación de propiedades no rechaza consistentemente un tipo incompatible.
- `src/api_index.cpp` conserva nombres de tipos, pero descarta parte de los metadatos necesarios para colecciones tipadas y validaciones detalladas.
- Los errores del puente se convierten en rechazos que pierden su código original. El wrapper JavaScript añade líneas que deben descontarse al informar ubicaciones.
- Los PackedStringArray probados siguen siendo reutilizables entre llamadas; falta inspección de valores, no se demostró una caducidad inmediata.
- La API base enumera 39 tipos concretos de Variant, desde NIL hasta PACKED_VECTOR4_ARRAY. VARIANT_MAX es un centinela, no otro tipo.

## Goals / Non-Goals

**Goals:**
- Clasificar los 39 tipos: formato contextual, formato explícito, salida, validación y límites de uso.
- Evitar pérdida silenciosa de datos y distinguir errores de conversión de errores del método llamado.
- Basar el contexto en firmas autorizadas y metadatos pasivos, no en nombres de propiedades ni en código por plugin.
- Conservar efectos y avances explícitos sin prometer rollback de untracked.

**Non-Goals:**
- No construir automáticamente recursos, scripts, nodos, RID o callbacks a partir de datos JSON.
- No deducir unidades, convenciones de rotación o valores faltantes mediante heurísticas.
- No probar métodos/getters/setters ni cargar scripts únicamente para averiguar sus tipos.
- No garantizar el éxito de una operación de dominio de un plugin solo porque la llamada nativa terminó sin error de invocación.
- No implementar este diseño ni modificar las especificaciones principales durante su captura.

## Decisions

### D1. Separar representación de datos, resolución del destino y conversión contextual

Flujo previsto:

```text
JS data / explicit tags / opaque references
                  |
       bounded passive data decoding
                  |
      authorized destination metadata
                  |
        validate and convert values
                  |
        dispatch allowed operation
                  |
       bounded result/error encoding
```

El worker valida datos JavaScript sin tocar objetos del motor. El hilo principal autoriza el destino y resuelve referencias y firmas vigentes. Antes de asignar o invocar, valida todos los argumentos del elemento actual con el mismo codec. El contexto conocido permite convertir; su ausencia conserva Array/Dictionary y exige etiquetas para otros tipos. No se carga un script para completar el contexto. Un constructor sin metadatos disponibles recibe únicamente valores cuya representación ya sea explícita o JSON ordinario; sus requisitos de firma restantes los comprueba la invocación autorizada de Godot.

La validación pasiva no ejecuta getters JavaScript, `toJSON`, constructores ni métodos del usuario. Se rechazan ciclos de entrada, accessors, funciones, promesas anidadas, símbolos, arrays dispersos y valores undefined en datos. Un resultado superior undefined por una función void sigue representándose como result:null; no es un fallo.

Alternativas descartadas: conversión global por la forma `{x,y,z}` (rompe Dictionary); conversión por nombres como `position` (no es genérica); dejar que Godot coaccione (produce los éxitos falsos observados); rechazar todo JSON no etiquetado (demasiada fricción).

### D2. Descriptor de tipo completo y pasivo

Conservar identificador Variant, class_name, usage, hint/hint_string, tipo de elementos o claves/valores cuando exista, posición de argumentos, defaults y varargs. No inventar nullability, tipos de elementos ni firmas para scripts no cargados. Separar tipo de valor de permisos, modo de ejecución y estado de los metadatos.

ClassDB y metadatos de scripts ya cacheados proporcionan el contexto; no se llama al `_get_property_list` de una instancia para provocar descubrimiento dinámico. Si el motor no ofrece un dato fiable, se informa como desconocido y no se aplica inferencia contextual para esa parte. Native, GDExtension y GDScript comparten la misma resolución.

### D3. Reglas comunes de forma y precisión

- Las estructuras matemáticas aceptan campos nombrados exactos. No aceptan arrays posicionales, campos adicionales o componentes faltantes. Las únicas omisiones estructurales previstas son Color.a=1 y metadatos opcionales de transporte, no coordenadas ni columnas implícitas.
- `$t` selecciona un tipo explícito y debe coincidir con el destino o una conversión expresamente definida. Un tag desconocido produce UNSUPPORTED_VARIANT_TYPE; uno mal formado, INVALID_VALUE_ENCODING. No se interpreta como un Dictionary corriente por accidente.
- Para Dictionary/Variant sin contexto concreto, los objetos nombrados sin tag conservan su significado de datos, incluso si parecen vectores. Los tags explícitos siguen resolviéndose recursivamente.
- No se convierten strings numéricos ni booleanos a números. Un entero exige valor integral exacto y rango; no hay truncado ni wraparound. Un Number fuera del rango entero seguro no se acepta como int64.
- `int` y PackedInt64Array admiten BigInt de JavaScript dentro del rango signed-64, normalizado en el wire como `{$t:"int",value:"<decimal>"}`. La salida usa number en el rango seguro y ese tag fuera de él. Los decimales del tag usan sintaxis decimal canónica, no notación científica.
- Los floats finitos usan números. `{$t:"float",value:"NaN"|"+Infinity"|"-Infinity"|"-0"}` preserva los casos que JSON no conserva; también se admite un value numérico finito explícito. El float32 permite su redondeo IEEE normal, pero no overflow silencioso. No se prometen conversiones lossless de float64 a float32.
- Los componentes matemáticos nombrados de entrada deben ser finitos y representables. Si un valor matemático devuelto contiene componentes no finitos que impiden su representación estructurada normal, se conserva como referencia opaca inspeccionable, no se convierten sus componentes a null. Un float escalar mantiene el formato especial descrito arriba.
- Los números ordinarios de JavaScript no preservan por sí solos la distinción nominal int/float de un Variant sin contexto. Las etiquetas explícitas y los descriptores del destino resuelven esa distinción; no se promete recuperarla del JSON ambiguo.
- La validación se completa para todos los elementos de una colección/estructura antes de construir o entregar ese argumento. Un error debe indicar la ruta precisa, por ejemplo `args[1].basis.z.y`.

### D4. Matriz exhaustiva de tipos

Estado de esta tabla: contrato objetivo, no lista de funcionalidades ya implementadas. En las filas estructurales, el formato explícito es `{$t:"<Tipo>", ...campos}` y la salida usa esa misma forma. Un valor opaco válido del tipo esperado también puede pasarse como argumento. Los ejemplos de campos omiten `$t` únicamente para mostrar la forma contextual.

`F` = componente numérico finito representable; `I32` = entero signed-32 exacto. Los objetos anidados se validan según el tipo de su campo, sin necesitar tags adicionales cuando existe ese contexto.

| # | Tipo Variant | Entrada contextual / explícita | Salida y reglas específicas |
|---|---|---|---|
| 1 | Nil | null | null; solo para destino Nil, Variant u objeto/posición que admita null; no sustituye componentes ausentes. |
| 2 | bool | true/false | boolean; no 0/1 ni strings. |
| 3 | int | Number entero seguro, BigInt signed-64 o tag int con value decimal | number seguro o tag int; rango signed-64 y precisión exacta. |
| 4 | float | number o tag float | número finito o tag especial; no conversión desde string salvo las constantes explícitas del tag. |
| 5 | String | string | string; no conversión implícita de objetos. |
| 6 | Vector2 | {x:F,y:F} | tag Vector2; dos componentes obligatorios. |
| 7 | Vector2i | {x:I32,y:I32} | tag Vector2i; nunca redondear decimales. |
| 8 | Rect2 | {position:Vector2,size:Vector2} | tag Rect2; conservar signos, sin normalizar el rectángulo. |
| 9 | Rect2i | {position:Vector2i,size:Vector2i} | tag Rect2i; validar rangos anidados. |
| 10 | Vector3 | {x:F,y:F,z:F} | tag Vector3; no inferir unidades. |
| 11 | Vector3i | {x:I32,y:I32,z:I32} | tag Vector3i. |
| 12 | Transform2D | {x:Vector2,y:Vector2,origin:Vector2} | tag Transform2D; x/y son columnas; no identidad ni origen implícitos. |
| 13 | Vector4 | {x:F,y:F,z:F,w:F} | tag Vector4; distinto de Quaternion aunque tenga los mismos campos. |
| 14 | Vector4i | {x:I32,y:I32,z:I32,w:I32} | tag Vector4i. |
| 15 | Plane | {normal:Vector3,d:F} | tag Plane; sin normalización automática. |
| 16 | Quaternion | {x:F,y:F,z:F,w:F} | tag Quaternion; todos obligatorios; no inferir Euler, normalizar ni intercambiar orden. |
| 17 | AABB | {position:Vector3,size:Vector3} | tag AABB; sin corregir signos ni dimensiones. |
| 18 | Basis | {x:Vector3,y:Vector3,z:Vector3} | tag Basis; columnas en orden x/y/z, sin ortonormalización. |
| 19 | Transform3D | {basis:Basis,origin:Vector3} | tag Transform3D; transformación completa obligatoria. |
| 20 | Projection | {x:Vector4,y:Vector4,z:Vector4,w:Vector4} | tag Projection; cuatro columnas; no arrays planos ni transposición implícita. |
| 21 | Color | {r:F,g:F,b:F,a?:F} | tag Color con cuatro canales; a=1 si se omite; sin clamp, incluido HDR >1, ni conversión de espacio de color. |
| 22 | StringName | string contextual o {$t:"StringName",value:string} | tag StringName para conservar identidad al reenviar a Variant; sin invocar código. |
| 23 | NodePath | string contextual o {$t:"NodePath",value:string} | tag NodePath; no buscar ni crear nodos, ni reinterpretar como ruta de archivo. |
| 24 | RID | solo referencia opaca de valor RID emitida por el servidor | referencia de valor; ni number ni {$t:"RID",id:...}; no fabricar recursos del servidor de render/física. |
| 25 | Object | handle de objeto validado, o null donde sea admisible | handle con clase nativa/script y validación de compatibilidad con la clase declarada; nunca construir desde {class:...}. |
| 26 | Callable | solo referencia opaca de valor Callable ya obtenida legítimamente | referencia; inspección pasiva de tipo/validez, no invocación ni construcción desde JSON. Los usos autorizados conservan los controles existentes de callbacks. |
| 27 | Signal | solo referencia opaca de valor Signal ya obtenida legítimamente | referencia; inspección pasiva, sin emitir ni conectar como efecto de convertir. |
| 28 | Dictionary | objeto JSON para claves string ordinarias; tag Dictionary con entries para claves generales o reservadas | objeto ordinario cuando es inequívoco; si no, tag Dictionary con entries [{key,value}]; preservar tipos de claves y rechazar colisiones tras convertir. |
| 29 | Array | array JS; tag Array con values y element_type si se declara explícitamente | array ordinario o tag Array tipado; validar cada elemento y conservar restricciones conocidas. |
| 30 | PackedByteArray | array de enteros 0..255 o {$t:"PackedByteArray",values:[...]} | tag inline o referencia paginable; sin clamp. |
| 31 | PackedInt32Array | array de I32 o tag con values | tag inline o referencia paginable; signed-32 exacto. |
| 32 | PackedInt64Array | array de enteros seguros/BigInt/tags int o tag con values | tag inline o referencia paginable; tags int para elementos fuera del rango JS seguro. |
| 33 | PackedFloat32Array | array numérico o tag con values, usando tags float para especiales | tag inline o referencia paginable; redondeo float32 definido y overflow rechazado. |
| 34 | PackedFloat64Array | array numérico o tag con values, usando tags float para especiales | tag inline o referencia paginable; sin pérdida por JSON de NaN/infinito/-0. |
| 35 | PackedStringArray | array de strings o tag con values | tag inline o referencia paginable; strings accesibles como datos. |
| 36 | PackedVector2Array | array de objetos Vector2 nombrados o tag con values | tag inline o referencia paginable; validar cada vector. |
| 37 | PackedVector3Array | array de objetos Vector3 nombrados o tag con values | tag inline o referencia paginable; validar cada vector. |
| 38 | PackedColorArray | array de objetos Color nombrados o tag con values | tag inline o referencia paginable; mismo contrato RGB/alfa/HDR. |
| 39 | PackedVector4Array | array de objetos Vector4 nombrados o tag con values | tag inline o referencia paginable; cobertura explícita que falta en el decoder de 0.2.1. |

Los arrays de colecciones no son arrays posicionales de componentes: `[{x:1,y:2,z:3}]` puede ser PackedVector3Array; `[[1,2,3]]` no es una representación válida de sus vectores.

Enums y bitfields no añaden tipos Variant: usan el entero y sus metadatos de firma. No se convierten strings de nombres de constantes en números automáticamente ni se rechazan valores solo porque no tengan un nombre enum conocido. Los recursos, nodos, scripts e instancias de plugins pertenecen a Object y se someten a los permisos existentes.

### D5. Rotaciones: tipo y semántica son contratos distintos

Node3D.rotation y global_rotation usan Vector3 de Euler en radianes; rotation_degrees y global_rotation_degrees usan Vector3 en grados. Node3D.quaternion usa Quaternion; basis usa Basis y transform usa Transform3D. La conversión solo materializa el tipo de destino y no cambia el orden Euler configurado, unidades, referencia local/global, escala o normalización.

Ejemplo válido futuro: `godot.set(node,"rotation_degrees",{x:0,y:90,z:0})`. El mismo objeto enviado a rotation significa 90 radianes en Y, no una petición de conversión a radianes. El codec preserva lo pedido; las reglas de la propiedad de Godot siguen vigentes y pueden normalizar o restringir valores por su propia semántica.

### D6. Tags, Dictionary y colecciones tipadas sin colisiones

Los marcadores `$t`, `__godotHandle` y `__godotValue` son reservados en sus posiciones de envelope. Para transportar un Dictionary literal que tenga esas claves, se usa el tag Dictionary con entries. Ese envelope construye datos literales y no vuelve a reinterpretar el Dictionary resultante como una referencia. La salida elige entries cuando las claves no son strings, hay marcadores reservados o es necesario conservar el tipo de claves/valores.

El tag Dictionary admite descriptores key_type/value_type para tipos disponibles de forma pasiva; el tag Array admite element_type. Declaraciones explícitas contradictorias con el destino se rechazan. Un objeto JSON no convierte la clave string "1" en clave int; se requiere entries con una clave entera. Claves distintas que colisionarían después de una conversión se rechazan, no se sobrescriben silenciosamente.

Los descriptores son datos de tipos y nunca una instrucción de cargar scripts, buscar objetos o conceder acceso. Colecciones recursivas nativas que no pueden representarse sin ciclos se mantienen como referencias de valor; no se recorre un grafo sin límite.

### D7. Referencias de objeto y de valor no son intercambiables

Se mantienen los campos __godotHandle y __godotValue, pero su contenido es opaco e incluye una generación del servicio para no resolver accidentalmente referencias antiguas tras reiniciar. No es un cambio del token HTTP ni del protocolo de sesión. Las referencias viven entre ejecuciones hasta liberación explícita, invalidación del objeto o parada del servicio; no se liga su vida al contexto JS fresco.

- Un método de objeto con una referencia de valor falla con REFERENCE_KIND_MISMATCH, no con un engañoso handle expired.
- Una referencia de objeto caducada falla con INVALID_HANDLE; una referencia de valor inválida, con INVALID_VALUE_REFERENCE.
- El tipo real se verifica en el registro del servidor, no se confía en el campo type recibido del cliente.
- Las referencias opacas solo pueden representar valores ya emitidos. No se aceptan RID numéricos, punteros ni descripciones de callbacks como capacidad.

API prevista: `await godot.values.inspect(ref)` devuelve tipo y datos pasivos como longitud o campos disponibles; `await godot.values.read(ref,{offset,limit})` devuelve una porción ordenada de colección o sus campos estructurados. Ambas son consultas del puente disponibles en read, sin llamadas de objeto implícitas ni getters. Callable/Signal solo exponen metadatos pasivos; no su ejecución. Si el tipo no tiene un formato seguro de expansión, read devuelve UNSUPPORTED_VALUE_OPERATION y la referencia permanece reenviable.

`await godot.values.release(ref)` retira una referencia de valor del registro del puente, sin llamar explícitamente a free sobre objetos prestados. Requiere untracked autorizado: retirar la última referencia retenida puede desencadenar limpieza/refcount de valores que capturan recursos u objetos, por lo que no se promete ausencia de efectos de destrucción ni rollback. No acepta un handle de objeto como sustituto.

Colecciones inline: hasta 256 elementos y 16 KiB codificados; al superar cualquiera de los dos límites, se devuelve una referencia. Las lecturas requieren offset >=0 y limit entre 1 y 256, y respetan 16 KiB de datos por página. Un elemento demasiado grande se conserva como referencia anidada cuando sea representable de ese modo; nunca se devuelve JSON roto. Referencias anidadas mantienen sus propios permisos y vida útil.

El registro de valores tiene cuotas de cantidad y memoria contabilizable. No se evictan referencias vivas silenciosamente; alcanzar la cuota rechaza la nueva retención con VALUE_REFERENCE_LIMIT. Las cuotas exactas se fijarán como constantes verificables al implementar, no se promete que limiten memoria GPU o recursos externos. La codificación de resultados también consume presupuesto: completar una llamada nativa no exime de límites al serializar su resultado.

### D8. Diagnósticos estables, sin inventar estado recuperable

Conservar el código original del puente y separar errores JS genuinos de fallos de conversión, permiso o invocación. Incluir, cuando exista, operation, class, member, argument, path, expected, received, batch_index y source {line,column,stack}. Las ubicaciones se refieren al código enviado por el usuario, descontando el wrapper. No sustituir un código útil por JS_REJECTED. No inventar líneas o columnas cuando QuickJS no las proporciona.

El constructor Error intrínseco del worker reserva prepareStackTrace=undefined y stackTraceLimit=32 como propiedades no escribibles ni configurables antes de ejecutar código del usuario. Esta restricción local, aprobada durante la implementación, impide que QuickJS ejecute formateadores o conversiones de límite controlados por el usuario al generar diagnósticos. No cambia permisos del puente ni archivos del proyecto. La lectura del stack usa el getter intrínseco capturado antes del código del usuario, no getters o toString reemplazados.

La conversión fallida no se registra como efecto completado. Una llamada que sí se despachó mantiene su estado y el alcance real de los efectos; una respuesta void conserva result:null. Los diagnósticos no incluyen automáticamente argumentos, contenido de archivos, punteros ni tokens. Los stacks son acotados y limitados al código de usuario y marcos del puente necesarios, sin volcar valores locales.

Se detectan promesas anidadas en resultados/checkpoints y se produce UNAWAITED_VALUE con ruta, en vez de serializarlas como {}. `return godot.call(...)` sigue siendo válido porque el wrapper async adopta esa promesa superior. Detectar una promesa sin await no implica que sus efectos pendientes no hayan comenzado; se mantienen cancelación y barrera para informar honestamente su resultado.

### D9. Checkpoints explícitos, locales y acotados

API prevista: `checkpoint(name,value)` es una función global síncrona de execute, no otra godot.* asíncrona ni una herramienta MCP nueva. Captura una instantánea de datos presentes, sin resolver promesas, ejecutar accessors ni consultar objetos del motor. Las referencias se guardan como referencias, no como copias de la escena.

La respuesta añade `checkpoints:[{name,value}]` ordenados, también ante una excepción posterior. Los nombres repetidos añaden entradas y no sustituyen valores previos. Límite inicial: 32 entradas, nombre de 1 a 128 bytes UTF-8, 64 KiB acumulados codificados y profundidad máxima 32. Excederlos lanza CHECKPOINT_LIMIT, sin añadir la entrada incompleta y conservando las anteriores. Datos no serializables fallan con ruta explícita. El helper existe solo durante esa ejecución.

result sigue siendo el retorno final, o null si no se produjo; no se capturan variables locales. partial describe efectos potencialmente persistentes de untracked, no la mera existencia de checkpoints. Un checkpoint tomado antes de un rollback write conserva una observación histórica, no prueba que ese estado siga existiendo.

El presupuesto global de respuesta debe conservar un envelope JSON válido y el diagnóstico primario. Resultados, páginas, stacks, checkpoints y listas de efectos tienen límites coordinados. Si se omiten datos por tamaño, se señalan explícitamente como truncados, con conteos de omisión; no se informa una lista parcial de efectos como completa.

### D10. Permisos y transacciones permanecen separados de la conversión

La conversión de un dato no concede autorización para leer una propiedad, ejecutar un método o instanciar su clase. Se conservan los filtros y tiers; no se modifica allowlist.json. read sigue limitado al contrato vigente de operaciones de consulta; reclasificar consultas estáticas de archivos queda para otro cambio.

write valida antes de mutar y revierte el journal compatible ante un error posterior. untracked valida igual, pero no promete rollback de efectos previos ni permite reintentar automáticamente una ejecución fallida. Una llamada untracked desconocida puede ejecutar código arbitrario autorizado; el codec no es un sandbox para ese código.

Cada elemento de batch usa exactamente la misma validación. Este cambio conserva resultados por elemento y continuación de hermanos; no añade un nuevo modo atómico ni borra efectos de elementos previos. Las validaciones puras no deben consultar estado mutable ni provocar una llamada extra por componente.

### D11. Descubrimiento y ayuda accesibles desde MCP

Mantener solo search y execute. Añadir en el contexto search `api.types()` y `api.describeType(name)` para consultar el inventario, ejemplos de campos, formatos explícitos, formas contextuales, límites y categorías opaque-only. api.describe conserva descriptores suficientes en propiedades, parámetros y retornos. Buscar/describir nunca carga ni instancia scripts.

Las descripciones de herramientas explican await, etiquetas, objetos nombrados, valores frente a objetos, checkpoints y read/write/untracked. Las declaraciones TypeScript reflejan los envelopes y funciones reales. Visibilidad en el catálogo no es permiso para ejecutar. Esta alineación documental no autoriza APIs que hoy estén denegadas ni cambia el handshake MCP.

## Risks / Trade-offs

- Mayor superficie del codec -> matriz de fixtures generada contra todos los tipos concretos y error/fallback opaco explícito para tipos futuros no reconocidos, nunca éxito supuesto.
- Etiquetas y claves de Dictionary pueden colisionar -> envelope entries literal, sin segunda interpretación de marcadores.
- Pérdida de precisión JS/int64/float32 -> rangos explícitos, tags lossless y pruebas de frontera en ambos sentidos; no prometer precisión que el formato destino no tiene.
- Cambios de salida StringName/NodePath o colecciones -> marcar el cambio de wire, conservar referencias válidas durante su generación y documentar inspección/consumo del formato nuevo.
- Firmas incompletas de plugins/scripts -> no inferir ni cargar para inspeccionar; requerir tipos explícitos y comprobar la llamada autorizada sin falsear sus efectos.
- Serialización o páginas grandes bloquean el editor -> límites por profundidad/tamaño/cantidad y pruebas de latencia contra 0.2.1, sin nuevos cruces por componente.
- Nuevas referencias retienen memoria -> cuotas y liberación explícita; no eviction invisible ni promesas de contabilizar recursos externos completos.
- Metadatos no describen la semántica del setter/plugin -> reportar validación e invocación, no afirmar éxito de dominio ni imponer igualdad de lectura posterior a setters que normalizan legítimamente.

## Migration Plan

1. Mantener v0.2.0/v0.2.1 y los ZIP existentes intactos. Ninguna versión nueva se publica ni etiqueta durante esta planificación.
2. Implementar mediante tareas y tests fallidos primero, preservando read/write/untracked y autorizaciones. La revisión debe comprobar cobertura de los 39 tipos, no solo los casos originales.
3. Actualizar ayuda MCP y tipos con los formatos efectivamente implementados; no anunciar tipos de construcción opaca como construibles desde JSON.
4. Ejecutar pruebas aisladas de entradas, salidas, errores, checkpoints, referencias y rollback; recompilar Linux y Windows. No afirmar ejecución Windows sin probarla allí.
5. Evaluar compatibilidad y elegir versión de publicación en una decisión posterior. Reiniciar el servicio invalida referencias de la generación anterior; no se reinterpreta una referencia vieja.
6. Para volver a 0.2.1, reinstalar el addon anterior con Godot cerrado. No se guardan cambios de escena, recursos ni configuraciones como efecto de migrar el codec.
