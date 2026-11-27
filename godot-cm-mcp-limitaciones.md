# Informe de pruebas: plugin MCP `godot-cm` v0.2.1

**Fecha:** 2026-11-26
**Entorno:** Godot 4.7.2 (Forward+, Jolt Physics), Terrain3D 1.1.0, Phantom Camera 0.12
**Endpoint:** `http://127.0.0.1:8765/mcp` (herramientas expuestas: `search` y `execute`)
**Proyecto de prueba:** `/home/godotmentor/Projects/test-mcp`

## Resumen

Se usó el MCP para construir de punta a punta una escena 3D: terreno montañoso con Terrain3D,
iluminación estilo Monument Valley, tres `PhantomCamera3D` fijas con cambio por tecla E, esferas
físicas que se lanzan al hacer clic sobre el terreno (colores Catppuccin Mocha, `queue_free` a los
20 s) y un HUD con contadores. Después se lanzó una escena de prueba con `play_custom_scene` que
simuló clics y la tecla E y guardó capturas e informe.

**El flujo funciona**: crear nodos, configurar recursos, escribir scripts, guardar escenas y
ejecutarlas. Los problemas encontrados son de ergonomía, de falta de visibilidad sobre el juego y
de restricciones de seguridad.

Todo se ejecutó en modo `untracked`. **No se probó el modo `write` con diario (journal)**, ni el
deshacer, ni el comportamiento con el editor cerrado o reiniciado.

---

## 1. Posibles bugs o asperezas (prioridad alta)

### 1.1 Fallo silencioso con tipos mal codificados
`godot.set(n, "position", {x:1, y:2, z:3})` devuelve éxito, pero deja el valor en `(0,0,0)`.
Pasa igual con `[7,8,9]` y con `{__godotValue:"Vector3", ...}`. Con `Color` también: un
`{r:1,g:0,b:0,a:1}` dejó `r:0`. Solo funcionó el formato `{$t:"Vector3", x, y, z}`, que se dedujo
del formato de salida porque no hay documentación.

- **Impacto:** el error pasa desapercibido y se descubre al leer el valor de vuelta.
- **Sugerencia:** rechazar con un error explícito las codificaciones desconocidas, y documentar
  `{$t:"..."}` como formato de entrada.

### 1.2 Caché de scripts obsoleta al recargar
Tras reescribir `main.gd` con `FileAccess`, `ResourceLoader.load(path)` con el modo de caché por
defecto devolvió el script anterior: `has_method("_spawn_at_screen_position")` daba `false`.
Con `CACHE_MODE_REPLACE` (2) se cargó correctamente.

- **Impacto:** la escena guardada referencia la ruta, así que el juego sí carga el archivo nuevo,
  pero el editor queda con un script desfasado y los chequeos de `has_method` mienten.
- **Sugerencia:** documentarlo, o exponer una utilidad de "recargar script".

### 1.3 Fallo de conexión del cliente MCP
Al iniciar la sesión, Claude Code reportó `godot-cm (CONNECTION_CLOSED)`. Aun así, el servidor
respondía bien por `curl`: `initialize` devolvía 200, con `MCP-Session-Id` y
`serverInfo: godot-cm 0.2.1`, y `tools/list` mostraba las dos herramientas. Godot escuchaba en
`127.0.0.1:8765`. El cliente no logró reconectar durante toda la sesión y se trabajó con `curl`
directo contra el endpoint, enviando la cabecera `MCP-Session-Id`.

- **Causa:** no determinada (cliente o servidor).
- **Pista:** posible condición de carrera al arrancar, porque el servidor ya estaba arriba cuando
  se verificó por `curl`.

### 1.4 Errores que pierden el contexto
Si una sentencia falla, la respuesta llega como `ok:false` con `result:null` y solo un mensaje en
`diagnostics`. Se pierde todo lo calculado antes. Hubo que envolver cada paso en `try/catch` para
saber dónde fallaba. Además, los retornos `void` no aparecen en el resultado.

- **Sugerencia:** devolver el resultado parcial junto con el error y el número de línea.

---

## 2. Restricciones de seguridad (decisiones razonables, con coste)

### 2.1 Singletons bloqueados
`ProjectSettings`, `Engine`, `OS`, `DisplayServer`, `ClassDB` y `EditorSettings` no están
disponibles (`singleton not allowed or class tier locked`). Permitidos: `EditorInterface`,
`Input`, `InputMap`, `RenderingServer`, `PhysicsServer3D`, `Time`, `ResourceSaver`,
`ResourceLoader`.

Consecuencias prácticas:
- No se pudieron crear acciones de input en `project.godot`, así que los scripts las registran
  en `_ready` con `InputMap`.
- No se pudo leer el nombre del proyecto, la escena principal ni el renderer.
- No hay forma de lanzar procesos.

### 2.2 Métodos denegados en objetos permitidos
| Método / propiedad | Objeto |
|---|---|
| `free` | cualquier `Node` |
| `get_open_scenes` | `EditorInterface` |
| `get_resource_filesystem` | `EditorInterface` |
| `get_texture` | `Viewport` (del editor) |
| `can_instantiate`, `get_class` | recurso de script |
| `get` / `set` de la propiedad `script` | `Node` (hay que usar `set_script`) |

Efecto lateral: como `free` está denegado, cada intento fallido deja un árbol de nodos huérfano
en la memoria del editor hasta cerrarlo.

### 2.3 El modo `read` es demasiado estricto
Llamar a `DirAccess.get_files_at` o `FileAccess.file_exists` falla en modo `read` con
`mutating operation in read mode`, aunque solo lean. Obliga a usar `untracked`, que no tiene
rollback. En toda la sesión `rollback_supported` fue `false`.

- **Sugerencia:** clasificar como lectura las llamadas estáticas de solo consulta de
  `DirAccess` y `FileAccess`.

---

## 3. Limitaciones técnicas

### 3.1 Handles opacos que caducan
Un `PackedStringArray` devuelto (por ejemplo un listado de `DirAccess`) llega como handle y expira
de inmediato (`handle expired or unknown`), así que no se puede leer. Además `godot.call` y
`godot.getSingleton` son asíncronos: sin `await` se obtienen `Promise` que se serializan como `{}`
sin avisar.

### 3.2 Latencia por llamada
Una llamada tarda unos 20 ms (50 llamadas a `get_height` ≈ 985 ms). Muestrear 1024 alturas del
terreno superó el presupuesto cooperativo (`cooperative execution budget exceeded`).

- **Consecuencia:** los bucles grandes deben ir dentro del juego en GDScript, no por el MCP.

### 3.3 Sin visibilidad sobre el juego en ejecución
Solo existen `play_current_scene`, `play_custom_scene`, `stop_playing_scene` e `is_playing_scene`.
No hay capturas del juego, ni salida de consola, ni lectura de propiedades del proceso.

Solución que funcionó: una escena de prueba (`test_run.tscn` + `test_driver.gd`, generada por el
propio MCP) que instancia el paisaje, simula clics y la tecla E con `Input.parse_input_event`,
guarda PNG e informe en disco y llama a `quit()`. Los logs de `godot.log` en
`~/.local/share/godot/app_userdata/TestMCP/logs/` se leyeron desde fuera.

---

## 4. Lo que funcionó bien

- Crear y empaquetar escenas (`PackedScene.pack` + `ResourceSaver.save`) y editar la escena
  abierta con `godot.editor.getEditedScene()` y `godot.saveScene()`.
- Clases de extensiones (Terrain3D, Phantom Camera) accesibles igual que las nativas, con
  `api.describe` y `api.find` para descubrir firmas.
- Importar heightmaps a Terrain3D y guardar sus datos con `save_directory`.
- Escribir scripts con `FileAccess` y asignarlos con `set_script`.
- Lanzar la escena con `play_custom_scene` y comprobar el estado con `is_playing_scene`.

## 5. Notas del propio uso (no son fallos del plugin)

- El estilo asíncrono con `await` en todas las llamadas se descubrió por prueba y error.
- Varios errores propios (por ejemplo `tonemapper` en lugar de `tonemap_mode`, o `Color.html`
  con dos argumentos) se detectaron por los mensajes del MCP y por el log del juego.

## 6. Sugerencias priorizadas

1. Rechazar con error las codificaciones de tipos no reconocidas (1.1) y documentar `{$t}`.
2. Devolver resultados parciales y la línea del error (1.4).
3. Permitir en modo `read` las consultas estáticas de `DirAccess` y `FileAccess` (2.3).
4. Documentar o automatizar la recarga de scripts con `CACHE_MODE_REPLACE` (1.2).
5. Investigar el `CONNECTION_CLOSED` inicial del cliente (1.3).
6. Ofrecer algún canal para leer la salida del juego, o una captura del viewport en ejecución (3.3).
7. Permitir `free` o `queue_free` sobre nodos creados por el propio script (2.2).
