# ZombieKing DS Manager（Go，纯 stdlib + lib/pq）
# 构建上下文 = ds-manager/ 目录（docker-compose build: ../ds-manager）
FROM golang:1.24 AS build
WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags "-s -w" -o /out/ds-manager .

FROM alpine:3.20
RUN adduser -D -H -u 10001 ds
USER ds
COPY --from=build /out/ds-manager /usr/local/bin/ds-manager
EXPOSE 8443
ENTRYPOINT ["ds-manager"]
