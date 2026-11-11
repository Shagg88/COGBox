# COGBox admin panel: runbook

For Shagg and KiltedKernel. You do not need to be a developer. Read this once. Come back to it when something looks wrong.

Open the panel at https://cogbox-mp.duckdns.org/admin/ and sign in with your name, password and the 6-digit code from your authenticator app.

## What the cards mean

- **Master server**: the part that lets players find lobbies and friends. Green means it is running. Red means it is down.
- **Coturn**: the part that helps players connect to each other (STUN only). Green or red, same as above.
- **Players online / lobbies**: how many people and open games there are right now.
- **Errors**: the last 20 problems the server saw. A few old ones are normal. A long list of new ones is not.
- **Lobbies in orange**: older than 2 hours. A lobby that old is usually stuck. It is not harmful.
- **Restarts**: how many times Docker had to restart a piece by itself. A number that keeps climbing means it is crash-looping. Do not keep pressing restart. Tell Flippage.
- **Logs**: the last 300 lines of what a piece printed. Good for screenshots when you ask for help.
- **Audit**: who signed in and who pressed what. It uses names only. No player details are stored here.

## When to restart what

- Lobbies are stuck, or search or friends look dead, but the card is green: restart **master server**.
- Players cannot connect to each other and the master server is fine: restart **coturn**.
- The page itself will not load: you cannot fix it from here. Wait for Flippage.

To restart: press the button, type the word `RESTART`, confirm.

## What a restart does to players

- Players already in a match stay connected. The match does not use this server.
- People searching for a lobby and the friends list drop for about 5 seconds. Then they come back by themselves.
- Restarting **coturn** briefly breaks new connections only.
- Prefer quiet times. Do not restart twice in a row. Give it a minute.

## Backups

Press **Backup now**. The server keeps the newest 14.

Do it:
- before Flippage updates the server (ask him if unsure),
- before you restart something that already looks broken,
- once a week if nobody else does.

## What NOT to do

- Do not share your password or your authenticator code. Each person has their own login.
- Do not press restart over and over.
- Do not restart when the restart count is climbing. Tell Flippage.
- Do not copy logs into public chats without a look. They are meant to hold tags only, but check first.
- Do not try to open the server in any other way (no SSH, no Docker commands) unless Flippage asked you to.

## When to wait for Flippage

- The page will not load, or the sign-in is refused and you are sure your details are right.
- A card stays red after one restart.
- The restart count keeps going up.
- You lost your authenticator app. Flippage resets the login.
- Anything not on this page.

## For Flippage: first setup on the server

The secret is generated on the server, in the deploy folder, and never printed:

    umask 077
    printf 'ADMIN_SHARED_SECRET=%s\n' "$(openssl rand -hex 32)" > admin.env

Check the mode is 600 with `ls -l admin.env`. Never `cat` it, never copy it into the repo (it is in `.gitignore`).

Then: merge the `/admin*` block from `deploy/Caddyfile.example` into the live Caddyfile, run `docker compose up -d --build`, and reload Caddy.

Add a person: `docker compose exec admin node dist/cli.js add-user <name>`. It prints a one-time password and an enrolment link. Reset: `reset-user`. Remove: `remove-user`.
