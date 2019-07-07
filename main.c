/*C*****************************************************************************
*
* Jacek Sobiecki 2018319609
*
*C*/

#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <semaphore.h> // include POSIX semaphores
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/msg.h>
#include "drone_movement.h"
#include "drone_movement.c"


#define PIPE_NAME "input_pipe"
#define DEBUG //remove this line to remove debug messages
//#define DEBUGDRONES //remove this line to remove debug messages


typedef struct {
	char name[20];
	int quantity;
} product;
typedef struct {
	double x, y;
	char name[20];
	product products[3];
} warehouse;
typedef struct {
	char name[20];
	int id;
	product product1;
	double x, y;
	int warehouseID;
} order;
typedef struct {
	//position
	double x, y;
	//destination
	double xd, yd;
	int chargeLevel;
	char state[20];
	order dOrder;
} drone;
typedef struct {
	double x;
	double y;
} base;
typedef struct {
	int ordersAttributed;
	int prodLoadedInWarehouses;
	int ordersDelivered;
	int productsDelivered;
	int ordersDiscarded;
	int avgTime;
} stats;
typedef struct {
	long mtype;
	order mOrder;
	int prodID;
	int a; //a=0 -> message from drone, a=1 -> supply message
} msq_msg;
typedef struct {
	long mtype;
} rsp_msg;

int maxLength = 0, maxHeight = 0;
int numberOfDrones = 0, initialCharge = 0, maxCharge = 0;
int supplyFreq = 0, quantity = 0, timeUnit = 0;
int numberOfWarehouses = 0;
warehouse *warehouses;
warehouse *temp;
drone *drones;
char prodList[100];
base bases[4];
stats *stats1;

pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t condChange = PTHREAD_COND_INITIALIZER;
sem_t *semMutex;
sem_t *statsMutex;
FILE *logFile;

int shmid, shmidStats;
int fd_named_pipe;
int mq_id;

time_t T;
struct tm tm;

int mainPID, centralPID;

//pipe
int number_of_chars;
fd_set read_set;
char str[256];
int orderID;
int runningDrones = 1;
int running = 1;


char *rtrim(char *str) {
	char *end;
	/* skip leading whitespace */
	while (isspace(*str)) {
		str = str + 1;
	}
	/* remove trailing whitespace */
	end = str + strlen(str) - 1;
	while (end > str && isspace(*end)) {
		end = end - 1;
	}
	/* write null character */
	*(end + 1) = '\0';
	return str;
}

void cleanup(int signum) {
	runningDrones = 0;
	running = 0;

	if (getpid() == centralPID) {
		struct timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;

		sem_wait(statsMutex);
		fprintf(logFile, "Unexecuted commands:\n");
		FD_ZERO(&read_set);
		FD_SET(fd_named_pipe, &read_set);
		while (select(fd_named_pipe + 1, &read_set, NULL, NULL, &timeout) > 0) {
			if (FD_ISSET(fd_named_pipe, &read_set)) {
				number_of_chars = read(fd_named_pipe, str, sizeof(str));
				str[number_of_chars - 1] = '\0'; //put a \0 in the end of string
			}
			fprintf(logFile, "%s\n", str);
		}
		sem_post(statsMutex);
	}

	if (getpid() == mainPID) {
		//wait for all child processes to finish
		for (int i = 0; i < numberOfWarehouses + 1; i++) {
			wait(NULL);
		}

		////////clean///////////
		//pipe
		close(fd_named_pipe);
		unlink(PIPE_NAME);
		// semaphores
		sem_close(semMutex);
		sem_unlink("Mutex");
		sem_close(statsMutex);
		sem_unlink("StatsMutex");
		//shared memory
		shmdt(warehouses);
		shmctl(shmid, IPC_RMID, NULL);
		shmdt(stats1);
		shmctl(shmidStats, IPC_RMID, NULL);
		//remove msg queue
		msgctl(mq_id, IPC_RMID, NULL);

		//log finish
		T = time(NULL);
		tm = *localtime(&T);
		fprintf(logFile, "%02d:%02d:%02d: Finish of the program\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
		fclose(logFile);
		exit(0);
	}

}

void signal1() {
	sem_wait(statsMutex);
	printf("Statistics:::::::::::::::::::::::::::::::::\n");
	printf("Total number of orders attributed to drones: %d\n", stats1->ordersAttributed);
	printf("Total number of products loaded in warehouses: %d\n", stats1->prodLoadedInWarehouses);
	printf("Total number of orders delivered: %d\n", stats1->ordersDelivered);
	printf("Total number of products delivered: %d\n", stats1->productsDelivered);
	printf("Total number of orders discarded: %d\n", stats1->ordersDiscarded);
	printf(":::::::::::::::::::::::::::::::::::::::::::\n");
	sem_post(statsMutex);
}

void signal2() {
	sem_wait(statsMutex);
	T = time(NULL);
	tm = *localtime(&T);
	fprintf(logFile, "%02d:%02d:%02d: Statistics:::::::::::::::::::::::::::::::::\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
	fprintf(logFile, "Total number of orders attributed to drones: %d\n", stats1->ordersAttributed);
	fprintf(logFile, "Total number of products loaded in warehouses: %d\n", stats1->prodLoadedInWarehouses);
	fprintf(logFile, "Total number of orders delivered: %d\n", stats1->ordersDelivered);
	fprintf(logFile, "Total number of products delivered: %d\n", stats1->productsDelivered);
	fprintf(logFile, "Total number of orders discarded: %d\n", stats1->ordersDiscarded);
	fprintf(logFile, ":::::::::::::::::::::::::::::::::::::::::::::::::::::::::::\n");
	stats1->ordersDiscarded = 0;
	stats1->productsDelivered = 0;
	stats1->ordersDelivered = 0;
	stats1->prodLoadedInWarehouses = 0;
	stats1->ordersAttributed = 0;
	sem_post(statsMutex);
}

void readConfig() {
	FILE *file;
	char string[100];

	if ((file = fopen("config.txt", "r")) == NULL) {
		printf("Error! opening file");
		// Program exits if the file pointer returns NULL.
		exit(1);
	}
	//------------------------------------------------------max length, height
	fgets(string, 100, file);
	sscanf(string, "%d, %d", &maxLength, &maxHeight);
	bases[0].x = 0;
	bases[0].y = 0;
	bases[1].x = maxLength;
	bases[1].y = 0;
	bases[2].x = maxLength;
	bases[2].y = maxHeight;
	bases[3].x = 0;
	bases[3].y = maxHeight;
	//------------------------------------------------------products names
	fgets(prodList, 100, file);
	//---------------------------number of drones, initial charge, max charge
	fgets(string, 100, file);
	sscanf(string, "%d, %d, %d", &numberOfDrones, &initialCharge, &maxCharge);
	//------------------------supply Frequency, quantity, time unit
	fgets(string, 100, file);
	sscanf(string, "%d, %d, %d", &supplyFreq, &quantity, &timeUnit);
	//---------number of warehouses
	fgets(string, 100, file);
	fgets(string, 100, file);
	sscanf(string, "%d", &numberOfWarehouses);
	temp = malloc(sizeof(warehouse) * numberOfWarehouses);
	for (int i = 0; i < numberOfWarehouses; i++) {
		fgets(string, 100, file);
		//Warehouse1 xy: 100, 200 prod: Prod_A, 10, Prod_D, 10, Prod_E, 20
		sscanf(string, "%s xy: %lf, %lf prod: %[^,], %d, %[^,], %d, %[^,], %d", temp[i].name, &temp[i].x, &temp[i].y,
			   temp[i].products[0].name, &temp[i].products[0].quantity, temp[i].products[1].name,
			   &temp[i].products[1].quantity, temp[i].products[2].name, &temp[i].products[2].quantity);
	}
	fclose(file);
}

void *droneFunc(void *t) {
	int my_id = *((int *) t);
#ifdef DEBUGDRONES
	printf("Drone %d created\n", my_id);
#endif
	while (1) {
		//check state
		pthread_mutex_lock(&mutex);
		if (drones[my_id].x == drones[my_id].xd && drones[my_id].y == drones[my_id].yd) {
			strcpy(drones[my_id].state, "Rest");
			pthread_cond_broadcast(&condChange);
			if (runningDrones == 0) {
#ifdef DEBUGDRONES
				printf("Drone %d exited\n", my_id);
#endif
				pthread_mutex_unlock(&mutex);
				pthread_exit(NULL);
			}
		}
#ifdef DEBUGDRONES
		printf("Drone %d state: %s\n", my_id, drones[my_id].state);
#endif

		while (drones[my_id].x == drones[my_id].xd && drones[my_id].y == drones[my_id].yd) {
			if (runningDrones == 0) {
				pthread_mutex_unlock(&mutex);
				pthread_exit(NULL);
			}

			pthread_cond_wait(&cond, &mutex);
		}

		strcpy(drones[my_id].state, "Move to load");
#ifdef DEBUGDRONES
		printf("Drone %d state: %s\n", my_id, drones[my_id].state);
#endif

		while (drones[my_id].x != drones[my_id].xd && drones[my_id].y != drones[my_id].yd) {
			int b = move_towards(&drones[my_id].x, &drones[my_id].y, drones[my_id].xd, drones[my_id].yd);
			if (b == -2) {
				perror("Error in move_towards\n");
				exit(1);
			}
			drones[my_id].chargeLevel--;
			pthread_mutex_unlock(&mutex);
			sleep(timeUnit / 1000);
			pthread_mutex_lock(&mutex);
		}
		strcpy(drones[my_id].state, "Loading");
#ifdef DEBUGDRONES
		printf("Drone %d state: %s\n", my_id, drones[my_id].state);
#endif

		msq_msg msqMsg;
		rsp_msg rspMsg;

		msqMsg.mOrder = drones[my_id].dOrder;
		msqMsg.mtype = drones[my_id].dOrder.warehouseID + 1;
		msqMsg.a = 0;
		pthread_mutex_unlock(&mutex);
		msgsnd(mq_id, &msqMsg, sizeof(msqMsg) - sizeof(long), 0);
		pthread_mutex_lock(&mutex);
		msgrcv(mq_id, &rspMsg, sizeof(rspMsg) - sizeof(long), drones[my_id].dOrder.id + 1000, 0);
		strcpy(drones[my_id].state, "Move to deliver");
		drones[my_id].xd = drones[my_id].dOrder.x;
		drones[my_id].yd = drones[my_id].dOrder.y;
#ifdef DEBUGDRONES
		printf("Drone %d state: %s\n", my_id, drones[my_id].state);
#endif

		while (drones[my_id].x != drones[my_id].xd && drones[my_id].y != drones[my_id].yd) {
			int b = move_towards(&drones[my_id].x, &drones[my_id].y, drones[my_id].xd, drones[my_id].yd);
			if (b == -2) {
				perror("Error in move_towards\n");
				exit(1);
			}
			drones[my_id].chargeLevel--;
			pthread_mutex_unlock(&mutex);
			sleep(timeUnit / 1000);
			pthread_mutex_lock(&mutex);
		}
		pthread_mutex_unlock(&mutex);
		//time for delivery
		sleep(timeUnit / 1000);
#ifdef DEBUGDRONES
		printf("Drone %d delivered order\n", my_id);
#endif
		//find nearest base
		int baseID = 0;
		pthread_mutex_lock(&mutex);
		for (int i = 1; i < 4; i++) {
			if (distance(drones[my_id].x, drones[my_id].y, bases[i].x, bases[i].y) <
				distance(drones[my_id].x, drones[my_id].y, bases[baseID].x, bases[baseID].y)) {
				baseID = i;
			}
		}
		drones[my_id].xd = bases[baseID].x;
		drones[my_id].yd = bases[baseID].y;
		strcpy(drones[my_id].state, "Return to the base");
#ifdef DEBUGDRONES
		printf("Drone %d state: %s\n", my_id, drones[my_id].state);
#endif

		int orderID = drones[my_id].dOrder.id;
		while (drones[my_id].x != drones[my_id].xd && drones[my_id].y != drones[my_id].yd &&
			   orderID == drones[my_id].dOrder.id) {
			int b = move_towards(&drones[my_id].x, &drones[my_id].y, drones[my_id].xd, drones[my_id].yd);
			if (b == -2) {
				perror("Error in move_towards\n");
				exit(1);
			}
			drones[my_id].chargeLevel--;
			pthread_mutex_unlock(&mutex);
			sleep(timeUnit / 1000);
			pthread_mutex_lock(&mutex);
		}
		pthread_mutex_unlock(&mutex);
	}
}

void *charger(void *t) {
	while (1) {
		for (int i = 0; i < numberOfDrones; i++) {
			pthread_mutex_lock(&mutex);
			if (strcmp(drones[i].state, "Rest") == 0) {
				drones[i].chargeLevel += 5;
				if (drones[i].chargeLevel > maxCharge)
					drones[i].chargeLevel = maxCharge;
			}
			pthread_mutex_unlock(&mutex);
		}
		sleep(timeUnit / 1000);
		if (runningDrones == 0) {
			pthread_exit(NULL);
		}
	}
}

void assignOrder(order order1) {

	//check which warehouse has a product
	int wId = -1;
	sem_wait(semMutex);

	for (int i = 0; i < numberOfWarehouses; i++) {
		for (int j = 0; j < 3; j++) {
			if (strcmp(warehouses[i].products[j].name, order1.product1.name) == 0 &&
				warehouses[i].products[j].quantity >= order1.product1.quantity) {
				wId = i;
			}
		}
	}
	//choose the nearest drone
	int droneId = -1;
	int batteries[numberOfDrones];
	pthread_mutex_lock(&mutex);

	for (int i = 0; i < numberOfDrones; i++) {
		//count needed battery
		double x = drones[i].x;
		double y = drones[i].y;
		int battery = 0;
		while (x != warehouses[wId].x && y != warehouses[wId].y) {
			int b = move_towards(&x, &y, warehouses[wId].x, warehouses[wId].y);
			if (b == -2) {
				perror("Error in move_towards\n");
				exit(1);
			}
			battery++;
		}
		while (x != order1.x && y != order1.y) {
			int b = move_towards(&x, &y, order1.x, order1.y);
			if (b == -2) {
				perror("Error in move_towards\n");
				exit(1);
			}
			battery++;
		}

		int baseId = 0;
		for (int j = 1; j < 4; ++j) {
			if (distance(order1.x, order1.y, bases[j].x, bases[j].y) <
				distance(order1.x, order1.y, bases[baseId].x, bases[baseId].y))
				baseId = j;
		}
		while (x != bases[baseId].x && y != bases[baseId].y) {
			int b = move_towards(&x, &y, bases[baseId].x, bases[baseId].y);
			if (b == -2) {
				perror("Error in move_towards\n");
				exit(1);
			}
			battery++;
		}
		batteries[i] = battery;

		if (droneId == -1) {
			if ((strcmp("Rest", drones[i].state) == 0 || strcmp("Return to the base", drones[i].state) == 0)
				&& drones[i].chargeLevel >= battery) {
				droneId = i;
			}
		} else {
			if ((strcmp("Rest", drones[i].state) == 0 || strcmp("Return to the base", drones[i].state) == 0)
				&& drones[i].chargeLevel >= battery
				&& distance(drones[droneId].x, drones[droneId].y, order1.x, order1.y) >
				   distance(drones[i].x, drones[i].y, order1.x, order1.y)) {

				droneId = i;
			}
		}
	}
	//if no drone has needed charge level
	if (droneId == -1) {
		for (int i = 0; i < numberOfDrones; i++) {
			if (droneId == -1) {
				if ((strcmp("Rest", drones[i].state) == 0 || strcmp("Return to the base", drones[i].state) == 0)
					&& batteries[i] <= maxCharge) {
					droneId = i;
				}
			} else {
				if ((strcmp("Rest", drones[i].state) == 0 || strcmp("Return to the base", drones[i].state) == 0)
					&& batteries[droneId] - drones[droneId].chargeLevel > batteries[i] - drones[i].chargeLevel
					&& batteries[i] <= maxCharge) {
					droneId = i;
				}
			}
		}
	}

	if (droneId == -1) {
		T = time(NULL);
		tm = *localtime(&T);
		fprintf(logFile, "%02d:%02d:%02d: No drone available! Order: %s no:%d discarded\n", tm.tm_hour, tm.tm_min,
				tm.tm_sec,
				order1.name, order1.id);
#ifdef DEBUG
		printf("%02d:%02d:%02d: No drone available! Order: %s no:%d discarded\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
			   order1.name, order1.id);
#endif
		sem_post(semMutex);
		pthread_mutex_unlock(&mutex);
	} else {
		order1.warehouseID = wId;
		drones[droneId].xd = warehouses[wId].x;
		drones[droneId].yd = warehouses[wId].y;
		drones[droneId].dOrder = order1;
		pthread_cond_broadcast(&cond);
		pthread_mutex_unlock(&mutex);
		sem_post(semMutex);
		sem_wait(statsMutex);
		stats1->ordersAttributed++;
		sem_post(statsMutex);
		T = time(NULL);
		tm = *localtime(&T);
		fprintf(logFile, "%02d:%02d:%02d: Order %s no:%d assigned to drone %d\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
				order1.name, order1.id, droneId);
#ifdef DEBUG
		printf("%02d:%02d:%02d: Order %s no:%d assigned to drone %d\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
			   order1.name, order1.id, droneId);
#endif
	}
}

void central() {
	centralPID = getpid();
	//pipe
	orderID = 1;
	T = time(NULL);
	tm = *localtime(&T);
	fprintf(logFile, "%02d:%02d:%02d: Process [%d] Central created\n", tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
#ifdef DEBUG
	printf("%02d:%02d:%02d: Process [%d] Central created\n", tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
#endif
	////////////create drones//////////////
	int newDroneNum = numberOfDrones; //to allow change of number of Drones
	drones = malloc(sizeof(drone) * numberOfDrones);
	long *id = malloc(sizeof(long) * (numberOfDrones + 1));
	pthread_t *threads = malloc(sizeof(pthread_t) * (numberOfDrones + 1));
	for (int j = 0; j < numberOfDrones; j++) {
		switch (j % 4) {
			case 0:
				drones[j].x = bases[0].x;
				drones[j].xd = bases[0].x;
				drones[j].y = bases[0].y;
				drones[j].yd = bases[0].y;
				break;
			case 1:
				drones[j].x = bases[1].x;
				drones[j].xd = bases[1].x;
				drones[j].y = bases[1].y;
				drones[j].yd = bases[1].y;
				break;
			case 2:
				drones[j].x = bases[2].x;
				drones[j].xd = bases[2].x;
				drones[j].y = bases[2].y;
				drones[j].yd = bases[2].y;
				break;
			case 3:
				drones[j].x = bases[3].x;
				drones[j].xd = bases[3].x;
				drones[j].y = bases[3].y;
				drones[j].yd = bases[3].y;
				break;
		}
		drones[j].chargeLevel = initialCharge;
		strcpy(drones[j].state, "Rest");
		id[j] = j;
		if ((pthread_create(&threads[j], NULL, droneFunc, &id[j])) != 0) {
			perror("Error on create\n");
			exit(1);
		}
	}
	//---------charger-------------
	if ((pthread_create(&threads[numberOfDrones], NULL, charger, &id[numberOfDrones])) != 0) {
		perror("Error on create\n");
		exit(1);
	}
	//---------Named Pipe----------
	// Creates the named pipe if it doesn't exist yet
	if ((mkfifo(PIPE_NAME, O_CREAT | O_EXCL | 0777) < 0)) {
		perror("Cannot create pipe: ");
		exit(0);
	}

	// Opens the pipe for reading
	if ((fd_named_pipe = open(PIPE_NAME, O_RDWR)) < 0) {
		perror("Cannot open pipe for reading: ");
		exit(0);
	}

	while (1) {
		FD_ZERO(&read_set);
		FD_SET(fd_named_pipe, &read_set);
		if (select(fd_named_pipe + 1, &read_set, NULL, NULL, NULL) > 0) {
			if (FD_ISSET(fd_named_pipe, &read_set)) {
				number_of_chars = read(fd_named_pipe, str, sizeof(str));
				str[number_of_chars - 1] = '\0'; //put a \0 in the end of string
			}
		}

		char *curLine = str;
		while (curLine && running==1) {
			char *nextLine = strchr(curLine, '\n');
			if (nextLine) *nextLine = '\0';

			if (strncmp("ORDER", curLine, 5) == 0) {
				char orderName[20];
				char productName[20];
				int quant;
				double x;
				double y;
				//ORDER Req 1 prod: A, 5 to: 300, 100
				sscanf(curLine, "ORDER %[^p] prod: %[^,], %d to: %lf, %lf\n", orderName, productName, &quant, &x,
					   &y);
				rtrim(orderName);
				if (!strstr(prodList, productName) || x > maxLength || y > maxHeight) {
					T = time(NULL);
					tm = *localtime(&T);
					fprintf(logFile, "%02d:%02d:%02d: Invalid command: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
							curLine);
#ifdef DEBUG
					printf("%02d:%02d:%02d: Invalid command: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
						   curLine);
#endif

				} else {
					order order1;
					product prod1;
					strcpy(prod1.name, productName);
					prod1.quantity = quant;
					order1.product1 = prod1;
					order1.x = x;
					order1.y = y;
					order1.id = orderID;
					orderID++;
					strcpy(order1.name, orderName);
					T = time(NULL);
					tm = *localtime(&T);
					fprintf(logFile, "%02d:%02d:%02d: Reception of order: %s no:%d\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
							order1.name, order1.id);
#ifdef DEBUG
					printf("%02d:%02d:%02d: Reception of order: %s no:%d\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
						   order1.name, order1.id);
#endif
					assignOrder(order1);
				}
			} else if (strncmp("DRONE SET ", curLine, 10) == 0) {
				sscanf(curLine, "DRONE SET %d\n", &newDroneNum);
				runningDrones = 0;
				T = time(NULL);
				tm = *localtime(&T);
				fprintf(logFile, "%02d:%02d:%02d: Number of drones changed| new number %d\n",
						tm.tm_hour, tm.tm_min, tm.tm_sec, newDroneNum);
#ifdef DEBUG
				printf("%02d:%02d:%02d: Number of drones changed| new number %d\n",
					   tm.tm_hour, tm.tm_min, tm.tm_sec, newDroneNum);
#endif
				break;
			} else {
				T = time(NULL);
				tm = *localtime(&T);
				fprintf(logFile, "%02d:%02d:%02d: Invalid command: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
						curLine);
#ifdef DEBUG
				printf("%02d:%02d:%02d: Invalid command: %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
					   curLine);
#endif
			}

			if (nextLine) *nextLine = '\n';
			curLine = nextLine ? (nextLine + 1) : NULL;
		}
		if (runningDrones == 0) {
			pthread_mutex_lock(&mutex);
			for (int i = 0; i < numberOfDrones; i++) {
				while (strcmp(drones[i].state, "Rest") != 0) {
					pthread_cond_wait(&condChange, &mutex);
				}
			}
			pthread_mutex_unlock(&mutex);

			pthread_cond_broadcast(&cond);
			for (int j = 0; j < numberOfDrones + 1; j++) {
				pthread_join(threads[j], NULL);
			}
			runningDrones = 1;
			if (running == 0) {
				msq_msg msqMsg;
				msqMsg.a = 2;
				for (int m = 0; m < numberOfWarehouses; m++) {
					msqMsg.mtype = m + 1;
					msgsnd(mq_id, &msqMsg, sizeof(msqMsg) - sizeof(long), 0);
				}
				exit(0);
			} else {
				numberOfDrones = newDroneNum;
				////////////create drones//////////////
				drones = malloc(sizeof(drone) * numberOfDrones);
				id = malloc(sizeof(long) * (numberOfDrones + 1));
				threads = malloc(sizeof(pthread_t) * (numberOfDrones + 1));
				for (int j = 0; j < numberOfDrones; j++) {
					switch (j % 4) {
						case 0:
							drones[j].x = bases[0].x;
							drones[j].xd = bases[0].x;
							drones[j].y = bases[0].y;
							drones[j].yd = bases[0].y;
							break;
						case 1:
							drones[j].x = bases[1].x;
							drones[j].xd = bases[1].x;
							drones[j].y = bases[1].y;
							drones[j].yd = bases[1].y;
							break;
						case 2:
							drones[j].x = bases[2].x;
							drones[j].xd = bases[2].x;
							drones[j].y = bases[2].y;
							drones[j].yd = bases[2].y;
							break;
						case 3:
							drones[j].x = bases[3].x;
							drones[j].xd = bases[3].x;
							drones[j].y = bases[3].y;
							drones[j].yd = bases[3].y;
							break;
					}
					drones[j].chargeLevel = initialCharge;
					strcpy(drones[j].state, "Rest");
					id[j] = j;
					if ((pthread_create(&threads[j], NULL, droneFunc, &id[j])) != 0) {
						perror("Error on create\n");
						exit(1);
					}
				}
				//---------charger-------------
				if ((pthread_create(&threads[numberOfDrones], NULL, charger, &id[numberOfDrones])) != 0) {
					perror("Error on create\n");
					exit(1);
				}
			}
		}
	}
}

void warehouseFunc(int i) {
	msq_msg msqMsg;
	rsp_msg rspMsg;
	T = time(NULL);
	tm = *localtime(&T);
	fprintf(logFile, "%02d:%02d:%02d: Process [%d] Warehouse %d created\n", tm.tm_hour, tm.tm_min, tm.tm_sec, getpid(),
			i);
#ifdef DEBUG
	printf("%02d:%02d:%02d: Process [%d] Warehouse %d created\n", tm.tm_hour, tm.tm_min, tm.tm_sec, getpid(), i);
#endif


	while (1) {
		msgrcv(mq_id, &msqMsg, sizeof(msqMsg) - sizeof(long), i + 1, 0);
		sem_wait(semMutex);
		if (msqMsg.a == 0) {
			int prodID = 0;
			for (int j = 1; j < 3; j++) {
				if (strcmp(warehouses[i].products[j].name, msqMsg.mOrder.product1.name) == 0)
					prodID = j;
			}
			warehouses[i].products[prodID].quantity -= msqMsg.mOrder.product1.quantity;
			sleep(timeUnit * msqMsg.mOrder.product1.quantity / 1000);
			rspMsg.mtype = msqMsg.mOrder.id + 1000;
			msgsnd(mq_id, &rspMsg, sizeof(rspMsg) - sizeof(long), 0);
		} else if (msqMsg.a == 1) {
			warehouses[i].products[msqMsg.prodID].quantity += quantity;
			sem_wait(statsMutex);
			stats1->prodLoadedInWarehouses += quantity;
			sem_post(statsMutex);
			T = time(NULL);
			tm = *localtime(&T);
			fprintf(logFile, "%02d:%02d:%02d: Supply | Warehouse %d updating stock of %s quantity: %d\n", tm.tm_hour,
					tm.tm_min, tm.tm_sec, i, warehouses[i].products[msqMsg.prodID].name,
					warehouses[i].products[msqMsg.prodID].quantity);
#ifdef DEBUG
			printf("%02d:%02d:%02d: Supply | Warehouse %d updating stock of %s quantity: %d\n", tm.tm_hour, tm.tm_min,
				   tm.tm_sec, i, warehouses[i].products[msqMsg.prodID].name,
				   warehouses[i].products[msqMsg.prodID].quantity);
#endif
		} else {
			sem_post(semMutex);
			exit(0);
		}
		sem_post(semMutex);
	}
}

int main() {
	signal(SIGINT, cleanup);
	signal(SIGUSR1, signal1);
	signal(SIGUSR2, signal2);

	mainPID = getpid();

	logFile = fopen("logFile.txt", "w");
	T = time(NULL);
	tm = *localtime(&T);
	fprintf(logFile, "%02d:%02d:%02d: Start of the program\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
#ifdef DEBUG
	printf("%02d:%02d:%02d: Start of the program\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
#endif
	T = time(NULL);
	tm = *localtime(&T);
	fprintf(logFile, "%02d:%02d:%02d: Process [%d] Simulation Manager created\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
			getpid());
#ifdef DEBUG
	printf("%02d:%02d:%02d: Process [%d] Simulation Manager created\n", tm.tm_hour, tm.tm_min, tm.tm_sec, getpid());
#endif


	readConfig();

	/////////////////////SHARED MEMORY///////////////////////////////
	//stats
	if ((shmidStats = shmget(IPC_PRIVATE, sizeof(stats), IPC_CREAT | 0766)) < 0) {
		perror("Error in shmget with IPC_CREAT\n");
		exit(1);
	}
	if ((stats1 = (stats *) shmat(shmidStats, NULL, 0)) == (stats *) -1) {
		perror("Shmat error");
		exit(1);
	}
	//warehouses
	if ((shmid = shmget(IPC_PRIVATE, sizeof(warehouse) * numberOfWarehouses, IPC_CREAT | 0766)) < 0) {
		perror("Error in shmget with IPC_CREAT\n");
		exit(1);
	}
	if ((warehouses = (warehouse *) shmat(shmid, NULL, 0)) == (warehouse *) -1) {
		perror("Shmat error");
		exit(1);
	}
	//put warehouses data read from config into shared memory
	for (int i = 0; i < numberOfWarehouses; ++i) {
		warehouses[i] = temp[i];
	}
	// Create semaphores
	//stats
	sem_unlink("StatsMutex"); //delete named semaphore
	statsMutex = sem_open("StatsMutex", O_CREAT | O_EXCL, 0700, 1);
	//processes
	sem_unlink("MUTEX"); //delete named semaphore
	semMutex = sem_open("MUTEX", O_CREAT | O_EXCL, 0700, 1);

	////////////////////MSG QUEUE////////////////////////
	mq_id = msgget(IPC_PRIVATE, IPC_CREAT | 0700);
	if (mq_id < 0) {
		perror("Error creating MQ");
		exit(1);
	}

	/////////////////////PROCESSES//////////////////////
	for (int i = 0; i < numberOfWarehouses + 1; i++) //
	{
		if (fork() == 0) {
			if (i == numberOfWarehouses) {
				///////////////central//////////////
				central();
			} else {
				///////////////warehouses////////////////////
				warehouseFunc(i);
			}
			exit(0);
		}
	}
	srand(time(NULL));
	msq_msg msqMsg;
	msqMsg.a = 1;
	int k = 1;
	while (1) {
		int i = rand() % 3;
		msqMsg.mtype = k;
		msqMsg.prodID = i;
		msgsnd(mq_id, &msqMsg, sizeof(msqMsg) - sizeof(long), 0);
		k++;
		if (k > numberOfWarehouses)
			k = 1;
		sleep(supplyFreq * timeUnit / 1000);
	}

	return 0;
}
