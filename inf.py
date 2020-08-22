import random

z = []
fp = open("nul", "w");

while True:
	sum = 0
	z.append(random.randrange(0, 3 ** 98))
	for u in z:
		fp.write(str(u) + " ");

