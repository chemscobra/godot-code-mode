# Proposal

## Why

Las pruebas de 0.2.1 confirmaron asignaciones incompatibles que devuelven éxito, diagnósticos que pierden su causa y valores empaquetados difíciles de inspeccionar. Corregir únicamente Vector2, Vector3 y Color dejaría el mismo problema en rotaciones, transformaciones, colecciones y otros tipos de Variant.

## What Changes

- Definir una matriz exhaustiva de los 39 tipos concretos de Variant de la API base Godot 4.7, separando conversión estructurada, conversión contextual y referencias opacas; `VARIANT_MAX` no es un tipo de datos.
- Convertir objetos con campos nombrados según el tipo declarado del destino. No inferir tipos para Dictionary, Variant o argumentos sin tipo, ni admitir arrays posicionales como vectores, colores o matrices. Mantener las etiquetas `$t` explícitas.
- Validar componentes, rangos, colecciones, referencias y metadatos antes de asignar o invocar. RGB será obligatorio; alfa será opcional y tendrá valor 1. No convertir unidades, normalizar rotaciones ni fabricar referencias.
- Aplicar un contrato común a propiedades, argumentos, constructores autorizados, batches y valores anidados. La ausencia de metadatos no autoriza ejecutar código para averiguarlos.
- Permitir inspección acotada de valores opacos e inline de colecciones pequeñas, distinguiendo referencias de objetos y referencias de valores.
- Conservar códigos originales, rutas de datos, contexto, línea y stack cuando estén disponibles. Añadir checkpoints explícitos, limitados y locales a cada ejecución, sin captura automática de variables o resultados de operaciones.
- Exponer formatos y límites mediante ayuda consultable desde las herramientas MCP existentes y declaraciones TypeScript.
- Alinear los contratos OpenSpec antiguos con la separación vigente entre descubrimiento, autorización, read, write y untracked, sin cambiar políticas de seguridad.
- **BREAKING**: entradas antes aceptadas mediante coerción silenciosa, referencias mal utilizadas y resultados no serializables producirán errores explícitos. Los clientes deberán distinguir el contenido inline de una colección de su referencia opaca.

Fuera de alcance: conexión/reconexión del cliente, cambios del transporte o sus credenciales, logs/capturas/estado del juego, recarga automática de scripts, nuevas concesiones de permisos, adaptadores por plugin y cambios de versión o publicación.

## Capabilities

### New Capabilities

Ninguna: se amplían capacidades existentes.

### Modified Capabilities

- `godot-editor-bridge`: matriz de tipos, conversión contextual validada, representaciones y referencias de valores inspeccionables.
- `code-execution`: diagnósticos conservados, checkpoints explícitos, serialización acotada y contrato read/write/untracked.
- `api-discovery`: metadatos de tipos suficientes y ayuda del codec accesible por MCP, separando visibilidad de permiso de ejecución.
- `editor-transactions`: validación previa y alcance del rollback limitado a write; efectos de untracked separados de checkpoints.

## Impact

Áreas previstas: `src/api_index.*`, `src/bridge.*`, `src/js_worker.*`, descripciones de herramientas en `src/mcp_service.cpp`, declaraciones y documentación del addon y pruebas aisladas en `tools/`. No se prevén dependencias nuevas ni modificaciones de `allowlist.json`. Se conservarán los formatos existentes válidos y se verificarán Linux y Windows durante la implementación futura. Este cambio solo contiene planificación; no afirma que las nuevas conversiones estén implementadas.
