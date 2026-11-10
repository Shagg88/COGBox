FROM node:22-bookworm-slim AS builder

WORKDIR /app

# Install build dependencies for native compilation on multi-arch targets (amd64 / arm64)
RUN apt-get update && apt-get install -y python3 make g++ && rm -rf /var/lib/apt/lists/*

COPY package*.json tsconfig.json ./
RUN npm ci

COPY src/ ./src/
RUN npm run build

FROM node:22-bookworm-slim AS runner

WORKDIR /app

ENV NODE_ENV=production
ENV PORT=443
ENV DB_PATH=/data/master.db

RUN apt-get update && apt-get install -y python3 make g++ && rm -rf /var/lib/apt/lists/*

COPY package*.json ./
RUN npm ci --only=production

COPY --from=builder /app/dist ./dist

VOLUME ["/data"]

EXPOSE 443 80

CMD ["node", "dist/server.js"]
