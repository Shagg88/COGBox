FROM node:22-bookworm-slim AS builder
WORKDIR /app
# native modules (better-sqlite3, argon2) may need a toolchain when no prebuilt binary matches
RUN apt-get update && apt-get install -y python3 make g++ && rm -rf /var/lib/apt/lists/*
COPY package*.json tsconfig.json ./
RUN npm ci
COPY src/ ./src/
RUN npm run build

FROM node:22-bookworm-slim AS runner
WORKDIR /app
ENV NODE_ENV=production
ENV ADMIN_DB=/data/admin.db
RUN apt-get update && apt-get install -y python3 make g++ && rm -rf /var/lib/apt/lists/*
COPY package*.json ./
RUN npm ci --omit=dev
COPY --from=builder /app/dist ./dist
COPY public/ ./public/
# the volume must be owned by the unprivileged user before it is mounted
RUN mkdir -p /data && chown node:node /data
USER node
VOLUME ["/data"]
EXPOSE 8080
CMD ["node", "dist/main.js"]
