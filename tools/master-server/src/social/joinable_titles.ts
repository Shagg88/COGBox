// Games whose join through an accepted invite was confirmed in a trace (social step 4, 2026-11-06): only these offer
// Join / invites; the others show presence only. Gears 2 confirmed 2026-11-07 (party and private match joins).
export const JOINABLE_TITLES: ReadonlySet<string> = new Set([
  '4D5307D5',   // Gears of War
  '4D53082D',   // Gears of War 2
  '4D5308AB',   // Gears of War 3
  '4D530A26',   // Gears of War: Judgment
]);
