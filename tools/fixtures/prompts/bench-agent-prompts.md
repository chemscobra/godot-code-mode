# Prompts de benchmark: MCP vs edición de archivos

Proyecto bajo prueba: `/home/godotmentor/Projects/test-mcp`
(editor Godot abierto con los addons instalados, sin escenas abiertas).

Dos variantes del mismo encargo para comparar uso de tokens. La spec es
idéntica palabra por palabra; solo cambia el bloque "RESTRICCIÓN DE
MÉTODO" del final. Copia el bloque completo de cada variante en una
sesión nueva de agente (contexto limpio) con cwd o workspace en
`/home/godotmentor/Projects/test-mcp`, y mide tokens in/out, turnos y
tiempo.

## Variante A — por MCP

```markdown
Crea una escena 3D estilo Monument Valley en este proyecto Godot 4.7, de punta a punta:

- Geometría de diorama: plataforma, escalones, un cubo, un prisma dorado
  emisivo y un mecanismo de pistón animado (aro torus + cilindro pistón
  que suba y baje en bucle).
- Iluminación pastel/isométrica: entorno con niebla suave, sol cálido,
  sombras suaves. Paleta Catppuccin Mocha
  (base #1e1e2e, mauve #cba6f7, peach #fab387, teal #94e2d5).
- Tres cámaras fijas que alternen al pulsar la tecla E (registra la
  acción de InputMap desde script en _ready si ProjectSettings está
  bloqueado).
- Esferas RigidBody3D que se lancen en arco al hacer clic sobre el
  suelo, color aleatorio de la paleta, queue_free() a los 20 s.
- HUD (CanvasLayer) con contadores: esferas lanzadas, esferas vivas,
  cámara activa (1/3).
- Escena principal en `res://monument_valley.tscn`.

Después crea `res://test_run.gd` que instancie la escena, simule 5 clics
y 3 pulsaciones de E, espere ~2 s de física, y escriba un reporte JSON
en `user://bench_report.json` con estas claves exactas (1 = ok, 0 = no):

- `scene_loads`: la escena instancia sin errores
- `terrain_present`: existe un nodo de terreno/suelo raycasteable
- `diorama_nodes`: plataforma + escalones + cubo + prisma presentes
- `piston_moves`: el pistón cambió de posición entre dos muestreos
- `cameras_cycle`: la cámara activa cambió tras cada una de las 3 teclas E
- `spheres_spawn`: se crearon 5 RigidBody3D tras los 5 clics
- `hud_updates`: el HUD refleja los contadores
- `no_errors`: sin errores en el log durante la simulación

Termina con exit code 0 solo si todas las claves valen 1. Ejecuta la
prueba y corrige hasta que pase.

RESTRICCIÓN DE MÉTODO: construye TODO usando exclusivamente las tools
MCP del servidor godot-cm (search/execute con JS contra el editor).
No escribas archivos .tscn/.gd/.tres con herramientas de archivos salvo
el script de prueba si no hay otra vía. Si una clase no existe en
ClassDB, repórtalo y usa la alternativa más cercana.
```

Config MCP para el agente (si no la toma del entorno):

```json
{"mcpServers": {"godot-cm": {
  "type": "http",
  "url": "http://127.0.0.1:8765/mcp",
  "headers": {"MCP-Session-Id": "6fbd66aea2dc1b4b1ceac34e3ae8da7f"}
}}}
```

> El session token cambia cada vez que reinicia el plugin/editor —
> cópialo del dock de godot-cm o de `user://godot_cm_session`.

## Variante B — edición tradicional

```markdown
Crea una escena 3D estilo Monument Valley en este proyecto Godot 4.7, de punta a punta:

- Geometría de diorama: plataforma, escalones, un cubo, un prisma dorado
  emisivo y un mecanismo de pistón animado (aro torus + cilindro pistón
  que suba y baje en bucle).
- Iluminación pastel/isométrica: entorno con niebla suave, sol cálido,
  sombras suaves. Paleta Catppuccin Mocha
  (base #1e1e2e, mauve #cba6f7, peach #fab387, teal #94e2d5).
- Tres cámaras fijas que alternen al pulsar la tecla E (registra la
  acción de InputMap desde script en _ready si ProjectSettings está
  bloqueado).
- Esferas RigidBody3D que se lancen en arco al hacer clic sobre el
  suelo, color aleatorio de la paleta, queue_free() a los 20 s.
- HUD (CanvasLayer) con contadores: esferas lanzadas, esferas vivas,
  cámara activa (1/3).
- Escena principal en `res://monument_valley.tscn`.

Después crea `res://test_run.gd` que instancie la escena, simule 5 clics
y 3 pulsaciones de E, espere ~2 s de física, y escriba un reporte JSON
en `user://bench_report.json` con estas claves exactas (1 = ok, 0 = no):

- `scene_loads`: la escena instancia sin errores
- `terrain_present`: existe un nodo de terreno/suelo raycasteable
- `diorama_nodes`: plataforma + escalones + cubo + prisma presentes
- `piston_moves`: el pistón cambió de posición entre dos muestreos
- `cameras_cycle`: la cámara activa cambió tras cada una de las 3 teclas E
- `spheres_spawn`: se crearon 5 RigidBody3D tras los 5 clics
- `hud_updates`: el HUD refleja los contadores
- `no_errors`: sin errores en el log durante la simulación

Termina con exit code 0 solo si todas las claves valen 1. Ejecuta la
prueba y corrige hasta que pase.

RESTRICCIÓN DE MÉTODO: construye TODO creando y editando archivos del
proyecto directamente (.tscn, .gd, .tres). No uses ningún servidor MCP
de Godot. Verifica con `godot --headless --path
/home/godotmentor/Projects/test-mcp` (import, check-only o ejecutando
el test) e itera hasta el PASS.
```

## Protocolo de medición

- Mismo modelo y misma cuenta para ambas variantes; sesión nueva por
  corrida (sin contexto residual).
- Variante A: editor abierto en `test-mcp` con el plugin godot-cm
  activo (es la fuente del MCP).
- Variante B: **cierra el editor** antes de la corrida — una segunda
  instancia de Godot sobre el mismo proyecto puede chocar con el lock
  o reescribir `.godot/` mientras el agente verifica headless.
- Mínimo 3 corridas por variante; el proyecto debe estar en el mismo
  estado inicial cada vez (sin escenas, addons instalados).
- Anota por corrida: input tokens, output tokens, cache tokens,
  nº de turnos/mensajes, nº de tool calls, tiempo total, y la fidelidad.
- Fidelidad mínima = score del `bench_report.json`: suma de claves en 1
  sobre 8 (8/8 = PASS completo). El archivo queda en
  `~/.local/share/godot/app_userdata/test-mcp/bench_report.json` tras la
  corrida (o corre `godot --headless --path ... -s res://test_run.gd`
  tú mismo para regenerarlo). Como ambas variantes escriben el mismo
  reporte, la fidelidad es comparable aunque el método difiera.
- Si una corrida se interrumpe por límite de cuota, descártala (no
  es comparable).
- Resultado esperado a tabular:

| corrida | variante | in tok | out tok | cache tok | turnos | tools | tiempo | fidelidad /8 |
|---|---|---|---|---|---|---|---|---|
| 1 | mcp | | | | | | | |
| 1 | file | | | | | | | |
