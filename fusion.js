const admin = require("firebase-admin");
const serviceAccount = require("./earlywarningsystem-53511-firebase-adminsdk-fbsvc-c822e3407e.json");

admin.initializeApp({
  credential: admin.credential.cert(serviceAccount),
  databaseURL: "https://earlywarningsystem-53511-default-rtdb.firebaseio.com",
});

// ---------------- Helpers ----------------
function clamp01(x) {
  return Math.max(0, Math.min(1, x));
}

function riskFromLevel(level) {
  const m = { Low: 0.20, Medium: 0.50, High: 0.80, Critical: 0.95 };
  return m[level] ?? 0.20;
}

function toLevel(r) {
  if (r >= 0.75) return "Critical";
  if (r >= 0.55) return "High";
  if (r >= 0.30) return "Medium";
  return "Low";
}

function norm(x, lo, hi) {
  if (hi <= lo) return 0;
  return clamp01((x - lo) / (hi - lo));
}

async function getLatest(path) {
  const snap = await admin.database().ref(path).orderByKey().limitToLast(1).once("value");
  const val = snap.val();
  if (!val) return null;
  const key = Object.keys(val)[0];
  return { key, ...val[key] };
}

// Debounce so you don’t fuse 3 times for the same 3-second cycle
let fuseTimer = null;
function scheduleFuse(triggeredBy) {
  if (fuseTimer) return;
  fuseTimer = setTimeout(async () => {
    fuseTimer = null;
    try {
      await fuseLandslideNow(triggeredBy);
    } catch (e) {
      console.error("❌ Fusion error:", e);
    }
  }, 500); // 0.5s debounce
}

// ---------------- Core Fusion ----------------
async function fuseLandslideNow(triggeredBy) {
  const fl = await getLatest("/floodData");
  const eq = await getLatest("/earthquakeData");
  const ls = await getLatest("/landslideData");

  let R_flood = fl?.risk ?? riskFromLevel(fl?.risk_level);
  let R_quake = eq?.risk ?? riskFromLevel(eq?.risk_level);
  let R_land_base = ls?.risk ?? riskFromLevel(ls?.risk_level);

  if (fl?.flood_detected === 1) R_flood = Math.max(R_flood, 0.85);
  if (eq?.earthquake_detected === 1 || eq?.vibration_detected === 1) R_quake = Math.max(R_quake, 0.85);
  if (ls?.landslide_detected === 1) R_land_base = Math.max(R_land_base, 0.85);

  const water_level_cm = Number(fl?.water_level_cm ?? 0);
  const rain_pct = Number(fl?.rain_intensity_percent ?? 0);

  const motion = Number(eq?.motion ?? 0);
  const vib = Number(eq?.vibration_detected ?? 0);

  const soil_raw = Number(ls?.soil_moisture ?? 0);

  // Normalizations (starting guesses)
  const water_n = norm(water_level_cm, 0, 50);   // tune later if needed
  const rain_n = clamp01(rain_pct / 100.0);
  const motion_n = norm(motion, 0, 0.30);        // matches your MOTION_HIGH=0.30
  const vib_n = vib ? 1 : 0;
  const soil_n = clamp01(soil_raw / 4095.0);     // ADC assumption

  // Gates
  const G_flood_to_land = clamp01(0.45 * water_n + 0.35 * rain_n + 0.20 * soil_n);
  const G_quake_to_land = clamp01(0.70 * motion_n + 0.30 * vib_n) * (0.5 + 0.5 * soil_n);

  // Coupling strengths
  const A = 0.35; // flood -> landslide
  const B = 0.40; // quake -> landslide

  const land_fused = clamp01(
    R_land_base +
    A * (R_flood * G_flood_to_land) +
    B * (R_quake * G_quake_to_land)
  );

  const why = [];
  if (R_flood >= 0.80) why.push("Flood risk is high");
  if (R_quake >= 0.80) why.push("Earthquake risk is high");
  if (water_n > 0.6) why.push("Water level is elevated");
  if (rain_n > 0.6) why.push("Rain intensity is high");
  if (soil_n > 0.7) why.push("Soil moisture is high");
  if (motion_n > 0.6 || vib_n === 1) why.push("Ground shaking/vibration detected");

  const payload = {
    triggeredBy,
    ts: Date.now(),
    landslide: {
      baseRisk: R_land_base,
      fusedRisk: land_fused,
      fusedLevel: toLevel(land_fused),
    },
    inputs: {
      flood: {
        risk: R_flood,
        risk_level: fl?.risk_level ?? null,
        flood_detected: fl?.flood_detected ?? null,
        water_level_cm,
        rain_intensity_percent: rain_pct,
      },
      earthquake: {
        risk: R_quake,
        risk_level: eq?.risk_level ?? null,
        earthquake_detected: eq?.earthquake_detected ?? null,
        vibration_detected: eq?.vibration_detected ?? null,
        motion,
      },
      landslide_base: {
        risk: R_land_base,
        risk_level: ls?.risk_level ?? null,
        pressure: ls?.pressure ?? null,
        soil_moisture: soil_raw,
      },
    },
    gates: { G_flood_to_land, G_quake_to_land },
    why,
  };

console.log("PAYLOAD ROOT KEYS:", Object.keys(payload));
console.log("PAYLOAD.LANDSLIDE:", payload.landslide);

  await admin.database().ref("/fusion/landslide/current").set(payload);
  await admin.database().ref("/fusion/landslide/history").push(payload);

  console.log("✅ Fusion updated:", payload.landslide.fusedLevel, payload.landslide.fusedRisk.toFixed(3));
}

// ---------------- Listeners ----------------
// limitToLast(1) ensures it won’t replay your whole history.
function watch(path, name) {
  admin.database().ref(path).limitToLast(1).on("child_added", () => {
    console.log("📥 New:", name);
    scheduleFuse(name);
  });
}

watch("/floodData", "flood");
watch("/earthquakeData", "earthquake");
watch("/landslideData", "landslide");

console.log("🟢 Fusion worker is running... (Ctrl+C to stop)");