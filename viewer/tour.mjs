const PERIOD_SECONDS = 48;
const RADIUS = 620;
const TARGET_HEIGHT = 25;

// The starting angle puts the generated terrain and its boxes in view immediately.
export function tourPose(seconds) {
  const angle = Math.PI / 4 + (seconds % PERIOD_SECONDS) * (2 * Math.PI / PERIOD_SECONDS);
  const y = 280 + 36 * Math.sin(2 * angle);
  return {
    x: RADIUS * Math.sin(angle),
    y,
    z: RADIUS * Math.cos(angle),
    yaw: angle,
    pitch: -Math.atan2(y - TARGET_HEIGHT, RADIUS),
  };
}
