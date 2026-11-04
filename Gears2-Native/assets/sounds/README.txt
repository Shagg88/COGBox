COGBox achievement popup sound
==============================

achievement.wav is a placeholder (two short notes, made by tools/make_achievement_chime.py).

To change the sound, replace achievement.wav with your own .wav file. To give one popup style its own sound,
add one of these next to it (the style's own file wins):

  achievement-x360.wav   Xbox 360 style
  achievement-xbox.wav   modern Xbox style
  achievement-ps5.wav    PS5 style

No rebuild is needed: the games read the file each time a popup shows. "Achievement sound: Off" or
"Hide achievement popups" in COGBox's settings turn the sound off.
