# COGBox status reporter

Keeps one message in the Discord `#server-status` channel up to date. Every 60 s it checks the services and edits
that one message through a webhook. If someone deletes the message, it posts a new one.

It shows:

- **Matchmaking server:** up or down, plus uptime. It never shows player or lobby counts, names or addresses.
- **STUN (UDP 3478):** whether it answers a binding request.
- **GitLab:** the latest release, the pipeline on the default branch, the newest commit and the open issue count. Checked every 5 minutes.
- **SourceForge:** the latest file, with a link.
- **GitHub:** on (`GITHUB_ENABLED=true` in the compose file), checked every 5 minutes (about 48 unauthenticated
  requests an hour, under GitHub's 60 per address). When it can't be reached, it shows "unavailable".

The colour is red when the matchmaking server or STUN is down, amber when another source has a problem, and green
otherwise. GitHub never changes the colour. "Last checked … ago" stops moving if the reporter itself stops.

## Setup on the server (Flippage)

1. In Discord, open `#server-status`, then Edit Channel → Integrations → Webhooks → New Webhook, and copy its URL.
2. In `~/cogbox-master` on the server, create the file without printing the URL:

       umask 077; read -rs URL; printf 'DISCORD_WEBHOOK_URL=%s\nSOURCEFORGE_PROJECT=cogbox\n' "$URL" > status.env; unset URL

   Paste the URL when it waits, then press Enter. Check the mode is 600 with `ls -l status.env`. Never `cat` it.
3. Start the service with `docker compose up -d --build status-reporter`.

Without `status.env` the service stays idle and logs one line saying so.

To turn GitHub off, set `GITHUB_ENABLED=false` in `docker-compose.oracle.yml` and run step 3 again.

## Dry run

`npm run build && npm run dry-run` prints the embed JSON and posts nothing.
