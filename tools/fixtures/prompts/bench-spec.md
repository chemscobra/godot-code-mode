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
y 3 pulsaciones de E, espere ~2 s de física, e imprima un reporte
PASS/FAIL (esferas creadas, cámara tras cada E, pistón en movimiento).
Ejecuta la prueba headless y corrige hasta que pase.
