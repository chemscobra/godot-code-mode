# Dogfood prompt: Monument Valley 3D (v0.2.1 baseline)

Prompt original de la prueba manual vs MCP sobre v0.2.1 (ver
godot-cm-mcp-limitaciones.md). Reproduce la escena que generó ~17.000
caracteres de `effects` — el caso que motivó las respuestas compactas.
Sirve como dogfood 3D de referencia en la ronda de géneros.

## Prompt

Crea una escena 3D estilo Monument Valley en Godot 4.7, de punta a punta:

- Terreno montañoso con Terrain3D.
- Iluminación pastel/isométrica (entorno con niebla suave, sol cálido,
  sombras suaves).
- Geometría de diorama: plataforma, escalones, un cubo, un prisma dorado
  emisivo y un mecanismo de pistón animado (assembly con aro torus y
  cilindro pistón que suba y baje).
- Tres PhantomCamera3D fijas que alternen al pulsar la tecla E
  (registrar la acción de input desde script en _ready, ya que
  ProjectSettings está bloqueado).
- Esferas físicas que se lancen al hacer clic sobre el terreno, con la
  paleta Catppuccin Mocha; queue_free a los 20 s.
- HUD con contadores (esferas lanzadas, vivas, cámara activa).

Después prepara una escena de prueba que simule los clics y la tecla E,
ejecútala y guarda capturas y un informe del resultado.

## Qué estresa

- Volumen: docenas de create/set/add_child → factura de `effects` y
  `changes` (compact vs verbose).
- Recursos: StandardMaterial3D, Environment, primitivas de malla,
  PackedScene + ResourceSaver (tier filesystem).
- Scripts: attach script a MeshInstance3D del pistón y al nodo principal
  (hoy vía godot.attachScript; antes requería FileAccess + class_name).
- Singletons/tiers: InputMap (permitido), ProjectSettings (denegado a
  propósito), ResourceSaver (filesystem).
- Playback: scene.run + errores de runtime (hoy console: unavailable —
  limitación documentada).
