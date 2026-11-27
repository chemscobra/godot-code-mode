# Design

## Context

Véase `proposal.md` para motivación y alcance. El diseño es necesario por los riesgos de invalidación, costes en el hilo principal y compatibilidad del codec.

Base funcional: `v0.3.0` (`b483d41`) y los deltas completos de `improve-bridge-data-reliability`. Las especificaciones principales aún contienen descripciones antiguas de modos, scripts y tipos; no son autorización para volver a ellas. Este cambio agrega requisitos de rendimiento sin sincronizar ni archivar otros cambios.

Observaciones del código y las pruebas actuales:
- `Bridge::execute_op` llama a `ApiIndex::refresh` antes de ejecutar operaciones sobre objetos; cada elemento de un batch pasa por esa ruta.
- `ApiIndex::refresh` enumera clases/singletons, recorre escenas y reconstruye metadatos de scripts cacheados antes de comparar la revisión. Un retorno temprano por catálogo idéntico no evita ese trabajo previo.
- `BridgePump::pump` también reconcilia el catálogo aproximadamente cada 100 ms. `extensions_reloaded` invalida el índice; no se ha demostrado que esta señal cubra todos los cambios posibles.
- `Bridge::value_operation` serializa cada elemento y el prefijo acumulado de una página. Los diccionarios recorren desde el inicio hasta `offset`.
- `JsWorker` obtiene datos pasivos, los serializa a texto JSON y los parsea de nuevo para construir resultados. El límite final vuelve a serializar el envelope.
- `Bridge::drain` comprueba su presupuesto después de una operación completa; un batch se ejecuta entero dentro de ella. El tiempo end-to-end no equivale al bloqueo del editor.
- El benchmark actual registra cinco muestras sin calentamiento explícito ni fases. Sus 296,770 ms no identifican todavía una causa dominante.

## Goals / Non-Goals

**Goals:**
- Atribuir coste antes de optimizar y demostrar mejora con pruebas de complejidad y mediciones comparables.
- Hacer que lecturas repetidas sobre un catálogo estable reutilicen metadatos, no repitan descubrimiento global por elemento.
- Construir páginas sin volver a codificar todos los prefijos, manteniendo límites exactos UTF-8 y retenciones transaccionales del codec.
- Conservar formatos, errores y garantías de 0.3.0, incluida la frescura necesaria para autorizar cada operación.

**Non-Goals:**
- No garantizar un máximo de milisegundos para llamadas arbitrarias de Godot o plugins ni hacerlas interrumpibles.
- No redefinir batch como operación atómica o repartida entre frames; su coste y sus pausas se medirán.
- No resolver acceso aleatorio profundo a Dictionary mediante cursores ni cambiar su semántica ante mutación.
- No compartir contextos JS entre ejecuciones ni retener todas las generaciones históricas del catálogo.
- No usar cifras históricas como SLA ni afirmar compatibilidad de ejecución Windows desde una compilación cruzada.

## Decisions

### D1. Instrumentación de desarrollo antes de cambiar rutas críticas

Añadir contadores y tiempos monotónicos opt-in en los puntos existentes; no otra herramienta MCP ni campos públicos de resultado. El harness recogerá agregados acotados de un canal de diagnóstico de desarrollo, sin registrar argumentos, resultados, código fuente, rutas del proyecto, tokens ni identificadores de objetos. Desactivada, la instrumentación no recorrerá datos ni ejecutará formateadores adicionales.

Fases: arranque/activación, preparación del contexto y snapshot, espera de cola, actualización de catálogo, autorización/resolución de firmas, conversión de entrada, invocación nativa, codificación de salida, construcción de página y envelope final. Separar tiempos inclusivos/exclusivos; no sumar intervalos solapados de hilos distintos como si fueran CPU secuencial. Medir también el pump completo, incluida la reconciliación posterior a drain, y el máximo intervalo de trabajo continuo del puente en el hilo principal.

Contadores mínimos: solicitudes de refresh, reconciliaciones globales realizadas, nodos observados, scripts reflejados, publicaciones de catálogo, aciertos/fallos de caché, bytes enviados a codificación JSON y número de codificaciones, elementos de página y retenciones/liberaciones. Los totales por ejecución/ventana bastan; no crear una traza por componente sin límite.

La primera compilación instrumentada se basará en el código funcional de 0.3.0, antes de aplicar optimizaciones. El binario etiquetado permanece intacto como referencia end-to-end; no se le atribuirán contadores internos que no implementa.

Alternativa descartada: atribuir el 38% al codec únicamente porque el cambio lo incorporó. Los costes se solapan con descubrimiento y transporte de datos.

### D2. Separar metadatos, publicación y permiso vigente

Dividir responsabilidades del índice: descubrimiento/reconciliación global, metadatos normalizados reutilizables y proyección/publicación del snapshot. Mantener generaciones explícitas de metadatos y autorización. No construir una revisión completa costosa solo para decidir que no se necesita actualizar.

En estado estable, las operaciones nativas conocidas reutilizarán los descriptores de la generación vigente. La observación dirigida del destino y la resolución de handles siguen en el hilo principal. La caché guardará descriptores/planes de conversión inmutables, nunca el resultado de autorizar un handle ni el resultado de validar un valor mutable.

Dependencias de validez: clase nativa, identidad del script y su revisión conocida, generación del catálogo y generación de autorización. Cambiar una dependencia invalida el material afectado antes de su siguiente uso. No usar solo la ruta o el nombre de clase como prueba de identidad.

Casos que deben cubrirse: script añadido/eliminado, carga autorizada que convierte metadatos parciales en completos, cambio de script de un objeto, recarga con la misma identidad, recarga de extensión y cambios de tiers/políticas. Conectar únicamente notificaciones verificadas de Godot, con alta/baja simétrica. Donde no haya una señal fiable, conservar comprobación dirigida o reconciliación conservadora; la optimización no depende de suponer que todos los cambios emiten una señal.

Una llamada autorizada con efectos desconocidos puede cambiar scripts o clases dentro del mismo batch. Antes del siguiente elemento se invalidará/revalidará conservadoramente lo afectado. Si la frescura no puede demostrarse, se prioriza comprobarla sobre un acierto de caché. Una actualización periódica no sustituye la comprobación necesaria para autorizar o convertir la siguiente operación.

Los snapshots ya fijados por `api.*` siguen siendo inmutables durante su solicitud; no se sustituyen en mitad de la consulta. La ejecución sigue comprobando permisos y existencia actuales. Las generaciones antiguas se conservan solo mientras una solicitud las retenga, sin una caché histórica creciente.

Alternativas descartadas: quitar todos los refresh sin reemplazo, cachear autorizaciones y congelar el catálogo durante un batch que puede modificarlo.

### D3. Coalescer sin cambiar la semántica de batch

La coalescencia evita descubrimientos globales redundantes entre elementos estables, no las comprobaciones de cada elemento. Mantener orden, un despacho principal, resultados por elemento, continuación de hermanos, contabilidad de operaciones, efectos y journal de 0.3.0.

Ningún componente Vector3/Transform3D genera un cruce de hilo adicional. Conservar resolución e invocación de objetos en el hilo principal. No aumentar presupuestos para disimular costes ni añadir un modo multiframe. Documentar el tiempo continuo medido y cualquier exceso del presupuesto cooperativo; no presentar ese presupuesto como preempción.

### D4. Medición incremental exacta de páginas

Codificar una vez cada elemento aceptado y contabilizar su representación JSON UTF-8, claves, comas y estructura del envelope. Reutilizar el dato codificado y hacer una comprobación final exacta; no serializar el prefijo completo tras cada inserción. El trabajo de dimensionado debe crecer con los bytes de los elementos visitados y del envelope final, no con la suma de sus prefijos.

Preservar inicialmente los márgenes de seguridad actuales y los campos de continuación. Incluir campos finales y cambios de longitud decimal al medir el envelope. Verificar caracteres multibyte, escapes, claves reservadas, tags especiales, límites de profundidad y elementos que pasan a referencias. Un cálculo por número de caracteres o una estimación de memoria no reemplaza la medida del JSON real.

La selección tentativa de un elemento mantiene checkpoint de retención: si no cabe, revertir solo las nuevas referencias que produjo. Conservar referencias anidadas legítimas, cuotas y ausencia de eviction. Los casos límite deben producir JSON válido con el mismo contrato de error y sin fugas.

Para Dictionary se seguirá declarando por separado el coste de alcanzar `offset`; no llamar O(tamaño de página) al coste total cuando hubo que saltar miles de claves. No añadir un snapshot de todas las claves o un cursor persistente como efecto lateral de esta fase.

### D5. Retirar conversiones JSON intermedias solo con equivalencia demostrada

Después de perfilar, reutilizar el Variant de datos obtenido por la decodificación pasiva del worker donde hoy se convierte a texto y vuelve a parsearse. Mantener la validación de serializabilidad y el control exacto de bytes; no devolver directamente objetos JS activos ni resolver promesas anidadas.

Comparar contra 0.3.0: int64/BigInt, NaN/infinito/-0, void, Unicode y escapes, claves reservadas, errores primarios, stacks, checkpoints y omisiones. Cualquier ruta que no pueda mantener esa equivalencia conserva el camino validado hasta disponer de prueba. No introducir un serializador JSON alternativo ni una dependencia para el primer intento.

### D6. Protocolo de benchmark reproducible

Ampliar el harness temporal existente para extraer `v0.3.0` sin checkout ni escritura sobre el addon de trabajo. Registrar commit/tag, compilación, motor, plataforma, renderer, tamaños de escena/scripts, flags, timeout y estado de instrumentación. El baseline y el candidato usan los mismos datos, permisos y solicitudes.

Matriz mínima:
- Batches de 1, 10, 100 y 500 lecturas; controles de set/call autorizados, errores y mezcla de tipos.
- Proyecto mínimo, proyecto con scripts cacheados/anónimos y escena temporal de 1000 nodos; registrar cantidades reales. Separar catálogo estable de invalidación efectiva.
- Packed pequeños/grandes, páginas de 16/64/256 elementos y Dictionary con offsets iniciales/profundos; medir serialización y salto por separado.
- Activación frente a disponibilidad del servicio, reposo sin tráfico y actualización de metadatos. Editor headless para repetibilidad y con render activo para intervalos de frames.

Para operaciones repetidas: al menos 10 calentamientos descartados y 60 muestras por caso en cada una de tres rondas con orden baseline/candidato alternado. Calcular mediana y p95 por ronda, guardar muestras y errores. Arranque frío se mide como procesos independientes, sin confundirlo con calentamiento de solicitudes. Las series de arranque tendrán al menos 10 procesos por variante y se reportarán por separado, sin atribuirles un p95 robusto con tan pocas muestras.

Las fases internas se miden con instrumentación; las comparaciones principales de latencia se repiten desactivándola en ambos lados. Registrar su sobrecoste por separado. No extender tiempos del producto ni excluir silenciosamente muestras fallidas; si un caso alcanza su presupuesto, reportarlo como tal. Cualquier timeout especial del benchmark se declara y se aplica igual a ambas variantes.

0.2.1 permanece como referencia histórica: su batch requiere una representación interna y carece de paginación pasiva. No mezclar esos resultados con una comparación compatible 0.3.0/candidato.

### D7. Criterios de salida y decisiones basadas en evidencia

Criterios obligatorios:
1. Suite funcional completa de 0.3.0 sin pérdida de validación, permisos, snapshots, aislamiento JS, cuotas, diagnósticos, checkpoints o rollback.
2. En batches de lecturas estables, los contadores demuestran que descubrimiento global/reflexión de scripts no se repiten por cada elemento. Distinguir el trabajo periódico independiente del provocado por el batch.
3. Las pruebas de páginas demuestran que no se recodifican prefijos acumulados y que sus límites UTF-8 y retenciones siguen siendo correctos.
4. Las mediciones del camino optimizado muestran mejora repetible de mediana; p95, ocupación principal, activación y reposo se revisan expresamente. No aprobar una regresión reproducible no explicada en los controles ni calificar un resultado inconcluso como mejora.
5. Memoria/cachés y referencias se estabilizan tras ciclos de invalidación, retención y liberación, sin crecimiento por generaciones abandonadas.

La mejora temporal se evaluará frente a la dispersión observada entre rondas, publicando valores crudos. Si el ruido impide decidir, repetir bajo condiciones controladas; no fijar un SLA absoluto ni elegir un porcentaje después para hacer pasar el resultado. Recuperar aproximadamente la referencia histórica de 215 ms es una orientación, no un criterio contractual sobre hardware distinto.

Una optimización sin ganancia demostrable se ajusta o se retira antes de dar su tarea por completa. Si los datos justifican cambiar el contrato de batch, el alcance se vuelve a discutir con el usuario en otro cambio, sin implementarlo aquí.

## Risks / Trade-offs

- Caché obsoleta tras una mutación no notificada -> comprobaciones dirigidas, invalidación conservadora y pruebas de cambios entre elementos; nunca cachear un permiso como propiedad permanente del objeto.
- Instrumentación que distorsiona resultados -> comparación final con perfil desactivado, contadores acotados y sobrecoste publicado.
- Contador de bytes distinto al encoder -> pruebas diferenciales UTF-8/escapes y comprobación final exacta; conservar el margen actual.
- Reducir trabajo global puede posponer descubrimiento -> reconciliación periódica y ruta síncrona dirigida antes de depender de información nueva; medir su latencia, no ocultarla.
- Retener demasiados descriptores/snapshots -> cachés ligadas a generaciones activas, pruebas de liberación y medición de memoria en ciclos.
- Batch o método nativo largo sigue bloqueando -> medir y declarar la limitación; no prometer interrupción ni replanificar operaciones a escondidas.
- Optimizar una muestra poco representativa -> matriz de escenas/scripts, controles de escritura/error y perfiles headless/render activo.

## Migration Plan

1. Mantener `v0.3.0` y sus binarios intactos como referencia; solo crear artefactos de planificación durante esta propuesta.
2. Al comenzar una implementación autorizada, capturar baseline y añadir instrumentación/regresiones antes de optimizar; aplicar catálogo y serialización en pasos verificables.
3. Regenerar declaraciones únicamente si su salida cambia por el generador; no anunciar API nueva. Actualizar mediciones existentes con condiciones y limitaciones reales.
4. Recompilar Linux y Windows, ejecutar integración Linux, comprobar formatos y declarar por separado si no hay ejecución Windows.
5. Revisar resultados antes de decidir versión, commit, etiquetas o distribución. Este cambio no publica ni fusiona en main.
6. Para descartar un candidato, volver al addon 0.3.0 con el editor cerrado; no convertir escenas ni persistir cachés como requisito de migración.
