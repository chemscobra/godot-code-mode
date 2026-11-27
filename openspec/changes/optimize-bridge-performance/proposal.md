# Proposal

## Why

La medición existente registra un batch de 100 lecturas en 296,770 ms frente a 214,553 ms en 0.2.1, pero solo contiene cinco muestras y no separa espera, trabajo del hilo principal y serialización. En 0.3.0 hay recorridos del catálogo por operación y serialización repetida de páginas que conviene medir y reducir sin perder la fiabilidad recién incorporada.

## What Changes

- Establecer 0.3.0 como baseline reproducible en proyectos temporales, con calentamiento, muestras crudas, mediana, p95, fases de ejecución y ocupación del hilo principal. Mantener 0.2.1 únicamente como comparación histórica con sus diferencias documentadas.
- Añadir observabilidad de desarrollo opt-in, acotada y sin valores de argumentos, resultados, credenciales ni contenido del proyecto. No añadir herramientas o campos al protocolo MCP.
- Reutilizar metadatos vigentes y separar actualización del catálogo, invalidación y autorización para que las lecturas estables no repitan un descubrimiento global por elemento. Cambios relevantes deben invalidar las cachés antes de reutilizarlas.
- Reducir serializaciones de prefijos y conversiones JSON intermedias, conservando formatos, validación pasiva, límites exactos UTF-8, cuotas, errores, checkpoints y rollback de 0.3.0.
- Medir las pausas de batches sin repartirlos entre frames ni prometer interrupción de llamadas nativas. Medir activación y reposo como controles de regresión, no rediseñar el arranque.

No se prevén cambios incompatibles de API. Fuera de alcance: nuevo planificador, batches cooperativos/multiframe, modo atómico nuevo, cursores de paginación, pooling de contextos JS, acceso a objetos Godot desde workers, relajación de permisos o límites, transporte/credenciales, adaptadores por plugin, observación del juego y publicación de versiones.

## Capabilities

### New Capabilities

Ninguna: se añaden garantías de coste, invalidación y observabilidad a capacidades existentes.

### Modified Capabilities

- `api-discovery`: reutilización de catálogo estable con invalidación verificable, preservando snapshots y descubrimiento pasivo.
- `godot-editor-bridge`: coste de construcción de páginas sin serialización acumulativa repetida, cachés sin autorización obsoleta y contrato de batch/hilo principal conservado.
- `code-execution`: observabilidad de coste de desarrollo, comparación reproducible y presupuestos/formatos conservados durante la optimización.

## Impact

Áreas previstas: `src/api_index.*`, `src/bridge.*`, `src/value_codec.*`, `src/js_worker.*`, conexiones de invalidación en `src/mcp_service.cpp`, `tools/test_generic_bridge.py` y `docs/benchmarks.md`. No se requiere una dependencia nueva ni editar `allowlist.json`.

La base funcional es el tag local `v0.3.0` y los contratos de `improve-bridge-data-reliability`, completado pero aún no sincronizado con las especificaciones principales. Este cambio no archiva ni sincroniza aquel trabajo y no restaura contratos antiguos. Los números actuales son evidencia inicial, no una causa confirmada ni un SLA. Esta propuesta contiene solo planificación; la instrumentación y las optimizaciones aún no están implementadas.
