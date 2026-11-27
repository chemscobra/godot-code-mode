# Tasks

Base de implementación: `v0.3.0` y contratos de `improve-bridge-data-reliability`. Todas las casillas están pendientes; crear estos artefactos no inicia implementación. Aplicar en orden: medir primero y verificar cada optimización antes de continuar.

## 1. Baseline reproducible antes de optimizar

- [x] 1.1 Ampliar `tools/test_generic_bridge.py` para seleccionar el baseline `v0.3.0` y extraerlo en un proyecto temporal sin checkout; verificar que el harness identifica revisión/binario y no modifica el addon de trabajo ni el proyecto del usuario.
- [x] 1.2 Incorporar calentamiento, muestras crudas, mediana/p95 y rondas alternadas según D6; añadir pruebas del cálculo estadístico y del registro de muestras fallidas, y documentar las opciones reales en `docs/benchmarks.md`.
- [x] 1.3 Capturar el baseline sin instrumentación para batches 1/10/100/500, packed/páginas, escenarios de scripts/1000 nodos, activación y reposo; verificar respuestas funcionales y entregar resultados con condiciones, errores y límites, sin confundir HTTP con ocupación principal.

## 2. Instrumentación acotada y atribución del coste

- [x] 2.1 Añadir contadores y tiempos opt-in en índice, puente, codec y worker, incluyendo espera, pump completo y fases inclusivas/exclusivas de D1; verificar con fixtures que el reporte distingue mantenimiento, invocación y codificación sin duplicar intervalos solapados.
- [x] 2.2 Conectar el harness al canal de desarrollo sin nuevas herramientas/campos MCP; probar perfil desactivado, datos privados marcadores y grandes cantidades de operaciones para verificar ausencia de contenido sensible, trazas ilimitadas o recorridos adicionales con perfil apagado.
- [x] 2.3 Capturar perfiles de una compilación solo instrumentada derivada de 0.3.0, antes de optimizar; comparar con el binario etiquetado sin perfil, documentar el sobrecoste y atribuir el trabajo repetido usando tiempos y contadores reales en `docs/benchmarks.md`.
- [x] 2.4 Añadir medición del intervalo de trabajo continuo y de frames en un editor renderizado temporal; verificar que el informe distingue latencia total, fases principales y llamadas nativas no interrumpibles, y documentar las condiciones reproducibles.

## 3. Catálogo reutilizable con invalidación correcta

- [x] 3.1 Añadir regresión de coste para batches nativos estables y controles de reposo; comprobar que detecta en 0.3.0 descubrimiento global/reflexión repetidos por elemento, separándolos del mantenimiento periódico.
- [x] 3.2 Separar reconciliación global, descriptores normalizados y publicación por generaciones en `src/api_index.*`; hacer pasar la regresión 3.1 conservando firmas, defaults, herencia, colecciones y ausencia de cargas/getters de descubrimiento.
- [x] 3.3 Cubrir invalidación por script añadido/eliminado, carga autorizada, cambio de script, recarga con la misma identidad y extensión recargada; probar notificaciones reales y fallback dirigido donde falten, con alta/baja de señales simétrica al activar/desactivar el servicio.
- [x] 3.4 Integrar reutilización de descriptores en `src/bridge.*` sin cachear autorizaciones ni valores validados; verificar cambios de tier/política, handles borrados, referencias liberadas, entrada mutable y mutación de metadatos entre elementos del mismo batch.
- [x] 3.5 Probar snapshots fijados durante publicación y liberación de generaciones antiguas tras finalizar solicitudes; verificar estabilidad de memoria/cachés en ciclos de invalidación, sin historial creciente ni metadatos parciales presentados como completos.
- [x] 3.6 Repetir la matriz afectada con perfil activado/desactivado y documentar en `docs/benchmarks.md` ganancia, coste de invalidación y límites; verificar mediana reproducible y revisar p95/ocupación/reposo según D7 antes de completar el bloque.

## 4. Páginas sin recodificar prefijos

- [x] 4.1 Añadir pruebas de contadores para páginas 16/64/256, límites UTF-8, escapes y campos finales; verificar que la regresión detecta el dimensionado por prefijos acumulados y que los casos frontera comparan contra JSON realmente codificado.
- [x] 4.2 Implementar dimensionado incremental y comprobación final exacta en `Bridge::value_operation`, preservando márgenes, orden y continuación; hacer pasar 4.1 con arrays, packed, campos estructurados y diccionarios sin sumar todos los prefijos.
- [x] 4.3 Cubrir elementos sobredimensionados, referencias anidadas, cuotas y rollback de retenciones tentativas; verificar que una página fallida no filtra referencias ni invalida las anteriores y que sus errores/JSON siguen el contrato de 0.3.0.
- [x] 4.4 Medir páginas y offsets profundos de Dictionary por separado del salto inicial, incluyendo Unicode y valores grandes; publicar resultados en `docs/benchmarks.md` y verificar mejora de codificación sin atribuir acceso aleatorio constante ni introducir cursores.

## 5. Resultados del worker sin viajes JSON redundantes

- [x] 5.1 Añadir comparación de semántica contra 0.3.0 para resultados/checkpoints con int64, especiales float, void, Unicode, claves reservadas y referencias; incluir pruebas negativas de getters, toJSON, ciclos, proxies y promesas anidadas, y verificar que distinguen pérdidas de precisión o ejecución activa.
- [x] 5.2 Reutilizar los datos pasivos en las rutas del worker identificadas por el perfil, eliminando conversiones texto/parse redundantes donde se demuestre equivalencia; hacer pasar 5.1 y las pruebas de límites exactos, diagnóstico primario, omisiones y efectos ya ejecutados.
- [x] 5.3 Comparar latencia y bytes codificados del worker con perfil encendido/apagado y documentar en `docs/benchmarks.md` cambios y rutas conservadas; verificar mejora repetible sin nuevos estados JS compartidos ni aumento de presupuestos.

## 6. Verificación transversal y revisión de resultados

- [x] 6.1 Ejecutar la suite aislada `python3 tools/test_generic_bridge.py` y los escenarios de los tres deltas; comprobar los 39 tipos, permisos, batch mixto, read/write/untracked, Undo/Redo, rollback fallido, cancelación, cuotas, checkpoints y aislamiento de solicitudes con todas las optimizaciones juntas.
- [x] 6.2 Ejecutar la matriz final y comparar baseline/candidato con los tamaños y repeticiones D6; revisar todos los criterios D7, comprobar estabilización de memoria y documentar regresiones o resultados inconclusos sin tratarlos como ganancias ni cerrar tareas pendientes sin evidencia.
- [x] 6.3 Recompilar Linux y Windows con los comandos de AGENTS.md tras los cambios nativos, verificar formatos y ejecutar integración Linux con el binario resultante; comprobar las declaraciones con TypeScript y declarar explícitamente si Windows no se ejecutó.
- [x] 6.4 Ejecutar `openspec validate optimize-bridge-performance --strict` y `git diff --check`; revisar que resultados y documentación coinciden con lo probado y que no cambiaron permisos, transporte, versiones ni contrato monoframe de batch.

## Workflow follow-up

- Revisar el plan con el usuario y comenzar apply únicamente tras una nueva autorización de implementación.
- Presentar mediciones y limitaciones antes de decidir versión, commit, etiqueta, ZIP o publicación; no modificar main ni hacer push en este cambio sin autorización.
- Si hiciera falta un batch multiframe o cursores, plantearlos en otro cambio con contrato y consentimiento separados.
- Sincronizar/archivar cambios solo en el flujo autorizado; las specs principales y el cambio de fiabilidad completado permanecen intactos durante esta planificación.
