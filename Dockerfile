FROM lichessbotdevs/lichess-bot:2026.1.11.1
COPY . /opt/sleepmind
WORKDIR /opt/sleepmind
RUN ls -la
RUN make both && cp ./build/sleepmind /usr/local/bin/sleepmind
WORKDIR /lichess-bot
copy quantised.bin /usr/local/bin/quantised.bin
RUN chmod +x /usr/local/bin/sleepmind

CMD ["/bin/sh", "-c", "python3 lichess-bot.py --config /config.yaml"]