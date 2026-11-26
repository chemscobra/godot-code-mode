# Tasks

Esta lista describe implementación futura. Ninguna casilla está completada por haber redactado los artefactos. Referencias: matriz D4 de design.md y escenarios de las cuatro especificaciones delta.

## 1. Inventario y metadatos pasivos

- [x] 1.1 Crear fixtures de cobertura para los 39 tipos concretos de Variant y sus categorías, excluyendo VARIANT_MAX; verificar que el test enumera exactamente el inventario de la API base y detecta un tipo nuevo no clasificado.
- [x] 1.2 Conservar identificador Variant, clase, usage/hints, defaults, varargs y restricciones de colecciones en los descriptores; verificar firmas nativas, de una extensión y de scripts cacheados, sin perder la instantánea inmutable.
- [x] 1.3 Representar información incompleta sin cargar scripts, instanciar clases ni consultar listas de propiedades activas de instancias; verificar con contadores/sentinelas que el descubrimiento y sus pruebas no ejecutan código de usuario.
- [x] 1.4 Documentar en los tipos y ayuda existentes la diferencia entre tipo declarado, metadatos desconocidos y autorización; verificar ejemplos de un parámetro Variant y una clase visible pero todavía no ejecutable.

## 2. Codec de datos y tipos matemáticos

- [x] 2.1 Añadir primero regresiones del éxito falso de set, tags inválidos y campos faltantes/desconocidos; verificar que fallan con 0.2.1 y pasan tras la corrección sin cambiar la propiedad ni registrar un efecto completado.
- [x] 2.2 Sustituir la conversión JSON temprana por decodificación pasiva acotada que preserve representaciones hasta conocer el destino; verificar rechazo de accessors, toJSON activo, funciones, símbolos, ciclos, arrays dispersos y undefined anidado sin ejecutarlos.
- [x] 2.3 Implementar validación de Nil, bool, String, int/float y sus tags, incluyendo BigInt/int64 y valores float especiales; verificar límites signed-64, Number seguro, fracciones inválidas, booleanos, strings numéricos y round trips sin pérdida de NaN/infinito/-0.
- [x] 2.4 Implementar Vector2/3/4 y sus variantes enteras con campos nombrados y tags; verificar rangos signed-32, rechazo de arrays posicionales y conservación de Dictionary/Variant sin inferencia.
- [x] 2.5 Implementar Color con RGB obligatorio, alfa opcional 1 y sin clamp ni conversión de espacio de color; verificar RGB incompleto, alfa explícito, HDR y límites de representabilidad.
- [x] 2.6 Implementar Rect2, Rect2i, AABB y Plane usando sus campos anidados declarados; verificar formas válidas, rutas de errores internas y ausencia de normalización de signos/normales.
- [x] 2.7 Implementar Quaternion, Basis, Transform2D, Transform3D y Projection; verificar columnas, campos obligatorios, rechazo de reinterpretación Vector4/Quaternion y ausencia de normalización, identidad implícita o transposición.
- [x] 2.8 Añadir pruebas y ejemplos de rotation/rotation_degrees, quaternion, basis y transform en la documentación existente; verificar que las unidades locales/globales y el orden Euler no son alterados por el codec.
- [x] 2.9 Implementar salida estructurada simétrica para los tipos matemáticos y fallback opaco si hay componentes no finitos no representables; verificar lectura y reenvío de cada tipo sin convertir componentes a null.

## 3. Colecciones y referencias

- [x] 3.1 Implementar StringName y NodePath contextuales/etiquetados y salida con identidad explícita; verificar reenvío a Variant sin resolver rutas ni objetos, y documentar la diferencia respecto a los strings devueltos en 0.2.1.
- [x] 3.2 Implementar Array/Dictionary ordinarios, tipados y envelopes entries con claves reservadas/no-string; verificar vacíos tipados, conflictos de descriptores, claves duplicadas tras conversión y escape literal de $t/__godotHandle/__godotValue.
- [x] 3.3 Implementar las diez familias Packed, incluida PackedVector4Array, con validación completa antes de entregar el argumento; verificar enteros, float32/float64, int64, strings, vectores y colores, identificando el índice del elemento inválido.
- [x] 3.4 Distinguir referencias de Object y de valores usando generación e identidad del servidor, sin cambiar credenciales HTTP; verificar clase compatible, tipo real, valores entre ejecuciones, referencias cruzadas y referencias antiguas tras reinicio.
- [x] 3.5 Mantener RID, Callable y Signal como referencias opacas legítimas, sin construir capacidades desde JSON; verificar rechazo de números RID y diccionarios de callbacks y regresiones de autorización de clases/miembros/callbacks.
- [x] 3.6 Implementar godot.values.inspect/read, límites inline de 256 elementos/16 KiB y paginación acotada; verificar PackedStringArray legible, rangos inválidos, elementos grandes, ciclos nativos y ausencia de invocaciones de objetos/getters.
- [x] 3.7 Implementar cuotas documentadas de referencias y memoria contabilizable, error VALUE_REFERENCE_LIMIT y release en untracked autorizado; verificar que no se evictan referencias vivas, que release invalida solo su referencia y que read no puede liberar valores.
- [x] 3.8 Actualizar las declaraciones y ejemplos existentes de colecciones, escapes, cuotas y referencias; verificar que las formas inline/opacas y REFERENCE_KIND_MISMATCH son consumibles sin tratar un valor como un objeto.

## 4. Diagnósticos y serialización de respuestas

- [x] 4.1 Preservar códigos originales del puente y contexto de operación/tipo/ruta/elemento batch; verificar TYPE_MISMATCH, INVALID_HANDLE, INVALID_VALUE_REFERENCE y PERMISSION sin reducción a JS_REJECTED ni volcado automático de argumentos.
- [x] 4.2 Capturar línea/columna/stack disponibles y compensar el wrapper async sin ejecutar formateadores de excepciones de usuario; verificar errores en la primera línea y en una línea conocida, además de ubicaciones ausentes.
- [x] 4.3 Rechazar promesas anidadas en resultados con UNAWAITED_VALUE y conservar la adopción de la promesa superior; verificar el objeto con promesa olvidada, return godot.call directo y los efectos de operaciones que ya se despacharon.
- [x] 4.4 Coordinar presupuestos de respuesta para result, diagnósticos, efectos y páginas, preservando JSON válido y conteos de omisión; verificar respuestas grandes, error primario conservado, valores no serializables y void válido con result:null.
- [x] 4.5 Documentar códigos y ejemplos de diagnósticos en la ayuda/declaraciones existentes; verificar que los ejemplos identifican el campo fallido sin prometer resultados de variables locales o éxito de dominio del plugin.

## 5. Checkpoints explícitos

- [x] 5.1 Implementar checkpoint(name,value) síncrono y checkpoints ordenados en la respuesta; verificar snapshots independientes, nombres repetidos y conservación antes de un fallo posterior.
- [x] 5.2 Aplicar límites de 32 entradas, nombres UTF-8 de 1..128 bytes, 64 KiB acumulados y profundidad 32; verificar CHECKPOINT_LIMIT en cada frontera, sin entrada incompleta y conservando las anteriores.
- [x] 5.3 Usar serialización pasiva y detección de promesas para checkpoints, sin expandir objetos del motor; verificar referencias como referencias, rechazo de accessors/ciclos y lista vacía al iniciar otra ejecución.
- [x] 5.4 Añadir ejemplos de checkpoints a la ayuda y tipos existentes; verificar que se distinguen de effects, result y rollback y que no escriben archivos ni capturan datos automáticamente.

## 6. Integración con modos, batches y descubrimiento

- [x] 6.1 Integrar el mismo codec en set, llamadas de instancia/estáticas, constructores autorizados y elementos batch; verificar entradas equivalentes y rechazo previo a invocación con firmas conocidas, sin cargas de scripts solo para inspección.
- [x] 6.2 Conservar read/write/untracked y los tiers existentes sin editar allowlist.json; verificar que las conversiones no conceden permisos, los valores inválidos no mutan y las APIs desconocidas no se reclasifican como lectura por su nombre.
- [x] 6.3 Integrar validación con journal, Undo/Redo y efectos untracked; verificar rollback tras conversión fallida, bloqueo si falla el rollback, hermanos batch independientes y efectos previos untracked sin reintento automático.
- [x] 6.4 Verificar checkpoints antes de rollback, observaciones read y referencias posteriormente eliminadas; comprobar que partial no se activa por checkpoints solos ni se presenta una observación histórica como estado actual.
- [x] 6.5 Exponer api.types y api.describeType mediante search y actualizar descripciones de search/execute y TypeScript; verificar los 39 tipos, formatos contextuales/etiquetados, async, límites y categorías opaque-only sin agregar herramientas MCP.
- [x] 6.6 Contrastar la documentación existente con los contratos implementados y los cambios incompatibles; verificar ejemplos ejecutables y que no se anuncian funciones pendientes, concesiones nuevas o seguridad de sandbox inexistente.

## 7. Verificación transversal antes de revisión

- [x] 7.1 Ejecutar la suite aislada extendida con cobertura positiva/negativa de las 39 filas y los escenarios delta; verificar también las regresiones originales de permisos, scripts, llamadas estáticas y constructores sin tocar el proyecto del usuario.
- [x] 7.2 Medir activación, latencia de un batch y serialización/paginación frente a 0.2.1; verificar límites documentados y ausencia de cruces por componente o expansión de colecciones sin presupuesto.
- [x] 7.3 Recompilar Linux y Windows con los comandos de AGENTS.md y ejecutar la integración Linux con los binarios resultantes; verificar formato de ambos binarios y declarar explícitamente si falta ejecución en Windows. Linux ejecutado; Windows x86-64 compilado y formato PE32+ verificado, sin ejecución en Windows.
- [x] 7.4 Ejecutar openspec validate improve-bridge-data-reliability --strict y git diff --check; verificar que código, tipos, ayuda y escenarios coinciden y que no se alteraron configuraciones de permisos ni transporte.

## Workflow follow-up

- Revisar los resultados con el usuario antes de decidir versión, etiquetas, ZIP o publicación.
- Sincronizar/archivar las especificaciones solo en el flujo posterior autorizado; esta planificación no modifica las specs principales.
- La investigación de conexión del cliente, la observación del juego, la recarga de scripts y la reclasificación de consultas estáticas de archivos siguen fuera de este cambio.
