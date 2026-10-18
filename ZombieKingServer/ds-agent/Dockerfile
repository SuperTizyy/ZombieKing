# ZombieKing ds-agent（Go，纯 stdlib）
# 构建上下文 = ds-agent/ 目录（docker-compose build: ../ds-agent）
# 注意：agent 需拉起 UE5 专用服务器进程，生产建议直接部署在 DS 主机（挂载 DS 二进制目录），
#       容器模式仅适用于 DS 二进制可被容器内执行的场景。
FROM golang:1.24 AS build
WORKDIR /src
COPY go.mod ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags "-s -w" -o /out/ds-agent .

FROM alpine:3.20
RUN adduser -D -H -u 10001 ds
USER ds
COPY --from=build /out/ds-agent /usr/local/bin/ds-agent
EXPOSE 8444
ENTRYPOINT ["ds-agent"]
