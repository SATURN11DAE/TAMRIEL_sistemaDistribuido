FROM ubuntu:22.04

WORKDIR /app

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y \
    build-essential \
    gcc \
    make \
    libmicrohttpd-dev \
    libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

COPY . /app

RUN make

EXPOSE 3001
EXPOSE 3002
EXPOSE 3003

CMD ["./servidor"]
