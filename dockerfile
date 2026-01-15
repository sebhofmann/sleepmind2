FROM lichessbotdevs/lichess-bot:2025.6.20.1

copy build/sleepmind /usr/local/bin/sleepmind
copy quantised.bin /usr/local/bin/quantised.bin
RUN chmod +x /usr/local/bin/sleepmind

CMD ["/bin/sh", "-c", "python3 lichess-bot.py --config config.yaml"]