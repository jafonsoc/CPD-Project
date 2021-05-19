#include <omp.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "gen_points.h"
#include <mpi.h>

int n_dims, n_procs, id;
long n_points, max_depth, diff;
MPI_Status status;
omp_lock_t attach_lock;

enum MESSAGES {
    TERMINATE = -1,
    PRINT = -2
};

enum TAGS {
    PTS = 1,
    ID = 2
};

typedef struct _node
{
    long id;
    long left;
    long right;
    double *center;
    double radius;
    struct _node *next;
} node_t;

node_t *attach_node(node_t *head, node_t *new) {
    new->next = head;

    return new;
}

#pragma region math

double quick_distance(double *pt1, double *pt2)
{
    double dist = 0.0;

    for (int d = 0; d < n_dims; d++)
        dist += (pt1[d] - pt2[d]) * (pt1[d] - pt2[d]);
    return dist;
}

double distance(double *pt1, double *pt2)
{
    double dist = 0.0;

    for (int d = 0; d < n_dims; d++)
        dist += (pt1[d] - pt2[d]) * (pt1[d] - pt2[d]);
    return sqrt(dist);
}

void mean(double *pt1, double *pt2, double *mean)
{
    for (long i = 0; i < n_dims; i++)
    {
        mean[i] = (pt1[i] + pt2[i]) / 2;
    }
}

void get_furthest_points(double **pts, long l, long r, double **a, double **b)
{
    long i;
    double dist, max_distance = 0.0;

    /* Lock b as first point in set and find a */
    *b = pts[l];
    for (i = l; i < r + 1; i++)
    {
        if ((dist = quick_distance(*b, pts[i])) > max_distance)
        {
            *a = pts[i];
            max_distance = dist;
        }
    }

    max_distance = 0.0;

    /* Find b */
    for (i = l; i < r + 1; i++)
    {
        if ((dist = quick_distance(*a, pts[i])) > max_distance)
        {
            *b = pts[i];
            max_distance = dist;
        }
    }
}

void distr_get_furthest_points(double **pts, MPI_Comm comm, long size, double *a, double *b) {
    long i;
    double dist, max_distance = 0.0;
    double possible_points[n_dims * n_procs]; 

    /* Lock b as first point in set of leader and broadcast it */
    if (!id) {
        memcpy(b, pts[0], n_dims * sizeof(double));
    }
    MPI_Bcast(b, n_dims, MPI_DOUBLE, 0, comm);
    
    for (i = 0; i < size; i++)
    {
        if ((dist = quick_distance(b, pts[i])) >= max_distance)
        {
            memcpy(a, pts[i], n_dims * sizeof(double));
            max_distance = dist;
        }
    }

    max_distance = 0.0;

    /* Calculate real a at leader */
    MPI_Gather(a, n_dims, MPI_DOUBLE, possible_points, n_dims, MPI_DOUBLE, 0, comm);
    if (!id) {
        for (int p = 0; p < n_procs; p++) {
            if ((dist = quick_distance(b, &possible_points[p * n_dims])) >= max_distance)
            {
                memcpy(a, &possible_points[p * n_dims], n_dims * sizeof(double));
                max_distance = dist;
            }
        }
    }

    max_distance = 0.0;

    /* Broadcast a and calculate b */
    MPI_Bcast(a, n_dims, MPI_DOUBLE, 0, comm);
    
    for (i = 0; i < size; i++)
    {
        if ((dist = quick_distance(a, pts[i])) >= max_distance)
        {
            memcpy(b, pts[i], n_dims * sizeof(double));
            max_distance = dist;
        }
    }

    max_distance = 0.0;

    /* Calculate real b at leader */
    MPI_Gather(b, n_dims, MPI_DOUBLE, possible_points, n_dims, MPI_DOUBLE, 0, comm);
    if (!id) {
        for (int p = 0; p < n_procs; p++) {
            if ((dist = quick_distance(a, &possible_points[p * n_dims])) >= max_distance)
            {
                memcpy(b, &possible_points[p * n_dims], n_dims * sizeof(double));
                max_distance = dist;
            }
        }
    }

    /* Broadcast b */
    MPI_Bcast(b, n_dims, MPI_DOUBLE, 0, comm);
}

/* Subtracts p2 from p1 and saves in result */
void sub_points(double *p1, double *p2, double *result)
{
    long i;

    for (i = 0; i < n_dims; i++)
    {
        result[i] = p1[i] - p2[i];
    }
}

/* Adds p2 to p1 and saves in result */
void add_points(double *p1, double *p2, double *result)
{
    long i;

    for (i = 0; i < n_dims; i++)
    {
        result[i] = p1[i] + p2[i];
    }
}

/* Computes inner product of p1 and p2 */
double inner_product(double *p1, double *p2)
{
    long i;
    double result = 0.0;

    for (i = 0; i < n_dims; i++)
    {
        result += p1[i] * p2[i];
    }

    return result;
}

/* Multiplies p1 with constant */
void mul_point(double *p1, double constant, double *result)
{
    long i;

    for (i = 0; i < n_dims; i++)
    {
        result[i] = p1[i] * constant;
    }
}

/* Projects p onto ab */
void project(double *p, double *a, double *b_a, double *common_factor, double *result)
{
    double product;

    sub_points(p, a, result);
    product = inner_product(result, common_factor);

    mul_point(b_a, product, result);
    add_points(result, a, result);
}

#pragma endregion

#pragma region qselect

#define SWAP(x, y)         \
    {                      \
        double *temp1 = x; \
        x = y;             \
        y = temp1;         \
    }

int less_than(double *p1, double *p2)
{
    for (long i = 0; i < n_dims; i++)
    {
        if (p1[i] < p2[i])
        {
            return 1;
        }
        else if (p1[i] > p2[i])
        {
            return 0;
        }
    }
    return 0;
}

long median_of_three(double **pts, double **projs, long l, long r)
{
    long m = (l + r) / 2;
    if (less_than(projs[r], projs[l]))
    {
        SWAP(projs[r], projs[l]);
        SWAP(pts[r], pts[l]);
    }
    if (less_than(projs[m], projs[l]))
    {
        SWAP(projs[m], projs[l]);
        SWAP(pts[m], pts[l]);
    }
    if (less_than(projs[r], projs[m]))
    {
        SWAP(projs[r], projs[m]);
        SWAP(pts[r], pts[m]);
    }
    return m;
}

long partition(double **pts, double **projs, long l, long r, long pivotIndex)
{
    double *pivotValue = projs[pivotIndex];

    SWAP(projs[pivotIndex], projs[r]);
    SWAP(pts[pivotIndex], pts[r]);
    long storeIndex = l;

    for (long i = l; i < r; i++)
    {
        if (less_than(projs[i], pivotValue))
        {
            SWAP(projs[storeIndex], projs[i]);
            SWAP(pts[storeIndex], pts[i]);
            storeIndex++;
        }
    }

    SWAP(projs[r], projs[storeIndex]);
    SWAP(pts[r], pts[storeIndex]);

    return storeIndex;
}

double *qselect(double **pts, double **projs, long l, long r, long k)
{
    /* This way of doing it uses less stack */
    while (1)
    {
        if (l == r)
        {
            return projs[l];
        }
        long pivotIndex = median_of_three(pts, projs, l, r);
        pivotIndex = partition(pts, projs, l, r, pivotIndex);
        if (k == pivotIndex)
        {
            return projs[k];
        }
        else if (k < pivotIndex)
        {
            r = pivotIndex - 1;
        }
        else
        {
            l = pivotIndex + 1;
        }
    }
}

/* Computes the median point of a set of points in a line */
long median(double **pts, double **projs, long l, long r, double *center_pt)
{
    long projs_size = (r - l + 1);
    long k = projs_size / 2;

    if (projs_size % 2 != 0)
    {
        memcpy(center_pt, qselect(pts, projs, l, r, k + l), sizeof(double) * n_dims);
    }
    else
    {
        qselect(pts, projs, l, r, k + l);

        /* Finds point immediately before kth point */
        double *current = projs[l];
        for (long i = l + 1; i < l + k; i++)
        {
            if (less_than(current, projs[i]))
            {
                current = projs[i];
            }
        }
        mean(current, projs[k + l], center_pt);
    }
    k--;
    return k;
}

#pragma endregion

#pragma region distributed

#define MEMSWAP(x, y)         \
    {                      \
        double temp[n_dims]; \
        memcpy(temp, x, n_dims * sizeof(double));             \
        memcpy(x, y, n_dims * sizeof(double));         \
        memcpy(y, temp, n_dims * sizeof(double));         \
    }

#define SWAPDOUBLES(x, y)         \
    {                      \
        double temp; \
        temp = x; \
        x = y; \
        y = temp; \
    }

int cmpdoubles(const void *a, const void *b)
{
    double p1 = *(double*)a;
    double p2 = *(double*)b;
    if (p1 > p2) return 1;
    else if (p1 < p2) return -1;
    else return 0;
}

int cmppoints(const void *a, const void *b)
{
    double *p1 = (double*) a;
    double *p2 = (double*) b;
    if (less_than(p2, p1)) return 1;
    else return -1;
}

/* Parallel sorting by regular sampling */
double *distr_sorting(double *pts, long *set_size, MPI_Comm comm) {
    double my_pivots[n_procs];
    double all_pivots[n_procs * n_procs];
    int counts[n_procs];
    int displacements[n_procs];
    int rec_counts[n_procs];
    int rec_displacements[n_procs];
    long size = *set_size;

    /* Sort own portion of the array */
    qsort(pts, size, n_dims * sizeof(double), cmppoints);

    /* Select pivots */
    for (long i = 0; i < n_procs; i++) {
        my_pivots[i] = pts[i * (size / n_procs) * n_dims];
    } 

    /* Collect pivots at leader */
    MPI_Gather(my_pivots, n_procs, MPI_DOUBLE, all_pivots, n_procs, MPI_DOUBLE, 0, comm);
    if (!id) {
        qsort(all_pivots, n_procs * n_procs, sizeof(double), cmpdoubles);
        for (long i = 1; i < n_procs; i++) {
            my_pivots[i - 1] = all_pivots[i * n_procs];
        }
    }

    /* Broadcast final pivots */
    MPI_Bcast(my_pivots, n_procs - 1, MPI_DOUBLE, 0, comm);

    /* Calculate counts and displacements and share them  */
    long current = 0, count = 0, sum = 0;
    for (long pivot = 0; pivot < n_procs - 1; pivot++) {
        while (current < size && pts[current * n_dims] < my_pivots[pivot]) {
            count += n_dims;
            current++;
        }
        displacements[pivot] = sum;
        counts[pivot] = count;
        sum += count;
        count = 0;
    }
    displacements[n_procs - 1] = sum;
    counts[n_procs - 1] = (size - current) * n_dims;
    MPI_Alltoall(counts, 1, MPI_INT, rec_counts, 1, MPI_INT, comm);
    sum = 0;
    for (long i = 0; i < n_procs; i++) {
        rec_displacements[i] = sum;
        sum += rec_counts[i];
    }

    /* Final distribution of sorted array */
    double *rec_buf = (double*) malloc(sum * sizeof(double*));
    MPI_Alltoallv(pts, counts, displacements, MPI_DOUBLE, rec_buf, rec_counts, rec_displacements, MPI_DOUBLE, comm);
    qsort(rec_buf, sum / n_dims, n_dims * sizeof(double), cmppoints);

    *set_size = sum / n_dims;
    return rec_buf;
}

void send_points(int target, MPI_Comm comm, double **pts, long size) {
    /* Send number of points */
    MPI_Send(&size, 1, MPI_LONG, target, PTS, comm);

    /* Send array of points */
    for (long i = 0; i < size; i++) {
        MPI_Send(pts[i], n_dims, MPI_DOUBLE, target, i, comm);
    }
}

int receive_points(int sender, MPI_Comm comm, double ***pts, long *size) {
    /* Receive number of points */
    MPI_Recv(size, 1, MPI_LONG, sender, PTS, comm, &status);

    /* Check if termination message */
    if (*size == TERMINATE) {
        return 1;
    } else {
        /* Alocate space for points */
        double *_p = (double*) malloc(n_dims * *size * sizeof(double));
        *pts = (double**) malloc(*size * sizeof(double*));
        for (long i = 0; i < *size; i++) {
            (*pts)[i] = &_p[i * n_dims];
        }
    }

    /* Receive array of points */
    for (long i = 0; i < *size; i++) {
        MPI_Recv((*pts)[i], n_dims, MPI_DOUBLE, sender, i, comm, &status);
    }

    return 0;
}

void distr_find_center(double *projs, long sort_size, long distr_size, double *center, MPI_Comm comm) {
    /* Each processor searches its portion for right indexes and sends back to leader for broadcast */
    int n_centers = distr_size % 2 == 1 ? 1 : 2;
    int has_centers[n_centers];
    long center_indexes[n_centers];
    long base;

    /* Init values */
    for (int i = 0; i < n_centers; i++) {
        has_centers[i] = 0;
        center_indexes[i] = n_centers == 1 ? distr_size / 2 : distr_size / 2 - 1 + i;
    }

    /* Each processor looks for center points and sends them to leader if found */
    if (!id) {
        double centers[n_centers][n_dims];
        for (int j = 0; j < n_centers; j++) {
            if (center_indexes[j] >= 0 && center_indexes[j] < sort_size) {
                has_centers[j] = 1;
                memcpy(centers[j], &projs[center_indexes[j] * n_dims], n_dims * sizeof(double));
            }
        }

        /* Tell next processor to start searching */
        MPI_Send(&sort_size, 1, MPI_LONG, 1, 1, comm);

        /* Accumulate centers at leader to calculate real center */
        memset(center, 0, n_dims * sizeof(double));
        for (int j = 0; j < n_centers; j++) {
            if (!has_centers[j]) MPI_Recv(centers[j], n_dims, MPI_DOUBLE, MPI_ANY_SOURCE, j, comm, &status);

            /* Fill center */
            for (int d = 0; d < n_dims; d++) {
                center[d] += centers[j][d];
            }
        }
        for (int d = 0; d < n_dims; d++) center[d] /= n_centers;
    } else {
        MPI_Recv(&base, 1, MPI_LONG, id - 1, id, comm, &status);
        long max = sort_size + base;
        for (int j = 0; j < n_centers; j++) {
            if (center_indexes[j] >= base && center_indexes[j] < max) {
                /* Send center to leader */
                MPI_Send(&projs[(center_indexes[j] - base) * n_dims], n_dims, MPI_DOUBLE, 0, j, comm);
            }
        }

        /* Tell next processor to start searching */
        if (id < n_procs - 1) MPI_Send(&max, 1, MPI_LONG, id + 1, id + 1, comm);
    }
    MPI_Bcast(center, n_dims, MPI_DOUBLE, 0, comm);
}

long distr_partition(double **pts, double *projs, long size, double *center) {
    long storeIndex = 0;

    for (long i = 0; i < size; i++)
    {
        if (projs[i] < center[0])
        {
            SWAPDOUBLES(projs[storeIndex], projs[i]);
            MEMSWAP(pts[storeIndex], pts[i]);
            storeIndex++;
        }
    }

    return storeIndex;
}

#pragma endregion

node_t *create_node(long id) {
    node_t *node = (node_t*) malloc(sizeof(node_t));
    node->id = id;
    node->center = (double*) malloc(n_dims * sizeof(double));
    node->radius = 0.0;
    node->next = NULL;

    return node;
}

void free_node(node_t *node) {
    free(node->center);
    free(node);
}

void free_list(node_t *head) {
    node_t *aux = head;

    while (aux->next) {
        node_t *t = aux;
        aux = aux->next;
        free_node(t);
    }

    free_node(aux);
}

void finish_tree(double **pts, node_t **nodes, double **projections, long l, long r, long node_id, long depth)
{
    node_t *node = create_node(node_id);

    /* It's a leaf */
    if (r - l == 0)
    {
        memcpy(node->center, pts[l], n_dims * sizeof(double));
        node->left = -1;
        node->right = -1;
        omp_set_lock(&attach_lock);
        *nodes = attach_node(*nodes, node);
        omp_unset_lock(&attach_lock);
        return;
    }

    double *a, *b;

    get_furthest_points(pts, l, r, &a, &b);

    /* Compute common factors to all projections */
    double b_a[n_dims];
    sub_points(b, a, b_a);
    double denominator = inner_product(b_a, b_a);
    double common_factor[n_dims];
    mul_point(b_a, 1 / denominator, common_factor);

    /* Project points onto ab */
    for (long i = l; i < r + 1; i++)
    {
        project(pts[i], a, b_a, common_factor, projections[i]);
    }

    /* Find median point and split; 2 in 1 GIGA FAST */
    long split_index = median(pts, projections, l, r, node->center);

    /* Compute radius */
    for (long i = l; i < r + 1; i++)
    {
        double dist = distance(node->center, pts[i]);
        if (dist > node->radius)
        {
            node->radius = dist;
        }
    }

    node->left = node_id + 1;
    node->right = node_id + 2 * (split_index + 1);

    /* Add new node to list */
    omp_set_lock(&attach_lock);
    *nodes = attach_node(*nodes, node);
    omp_unset_lock(&attach_lock);

    if (depth < max_depth || (depth == max_depth && omp_get_thread_num() < diff)) {
#pragma omp taskgroup
        {
#pragma omp task
            finish_tree(pts, nodes, projections, l, l + split_index, node->left, depth + 1);
#pragma omp task
            finish_tree(pts, nodes, projections, l + split_index + 1, r, node->right, depth + 1);
        }
    } else {
        finish_tree(pts, nodes, projections, l, l + split_index, node->left, depth + 1);
        finish_tree(pts, nodes, projections, l + split_index + 1, r, node->right, depth + 1);
    }
}

void build_tree(double **pts, MPI_Comm team, node_t **nodes, long my_set, long team_set, long node_id) {
    MPI_Comm_size(team, &n_procs);
    MPI_Comm_rank(team, &id);

    /* Alone in team, finish sequentially */
    if (n_procs == 1) {
        /* Allocate memory for projections */
        double **projections = (double **)malloc(my_set * sizeof(double *));
        double *proj = (double *)malloc(my_set * n_dims * sizeof(double));
        double *to_free = *pts;
        max_depth = (int)log2(omp_get_max_threads());
        diff = omp_get_max_threads() - (1 << max_depth);
        for (long i = 0; i < my_set; i++)
        {
            projections[i] = &proj[i * n_dims];
        }
        omp_init_lock(&attach_lock);
#pragma omp parallel
#pragma omp single
        {
#pragma omp task
            finish_tree(pts, nodes, projections, 0, my_set - 1, node_id, 0);
        }
        omp_destroy_lock(&attach_lock);
        free(projections);
        free(proj);
        free(to_free);
        free(pts);
        return;
    }

    node_t *node = create_node(node_id);

    /* Find a and b */
    double *a = (double*) malloc(n_dims * sizeof(double));
    double *b = (double*) malloc(n_dims * sizeof(double));

    distr_get_furthest_points(pts, team, my_set, a, b);
    
    /* Compute common factors to all projections */
    double b_a[n_dims];
    sub_points(b, a, b_a);
    double denominator = inner_product(b_a, b_a);
    double common_factor[n_dims];
    mul_point(b_a, 1 / denominator, common_factor);

    /* Allocate memory for projections */
    double *proj_first_coord = (double*) malloc(my_set * sizeof(double));
    double *proj = (double*) malloc(my_set * n_dims * sizeof(double));
    for (long i = 0; i < my_set; i++)
    {
        project(pts[i], a, b_a, common_factor, &proj[i * n_dims]);
        proj_first_coord[i] = proj[i * n_dims];
    }
    free(a);
    free(b);

    /* Sort first coordinate of projections */
    long sorted_set = my_set;
    double *sorted_projs = distr_sorting(proj, &sorted_set, team);

    /* Find center projection */
    distr_find_center(sorted_projs, sorted_set, team_set, node->center, team);
    free(proj);
    free(sorted_projs);

    /* Partition own portion of array */
    long split = distr_partition(pts, proj_first_coord, my_set, node->center); 
    free(proj_first_coord);

    /* Calculate radius */
    double max_distance = 0.0;
    for (long i = 0; i < my_set; i++)
    {
        double dist = distance(node->center, pts[i]);
        if (dist > max_distance)
        {
            max_distance = dist;
        }
    }
    MPI_Reduce(&max_distance, &node->radius, 1, MPI_DOUBLE, MPI_MAX, 0, team);

    node->left = node_id + 1;
    node->right = node_id + 2 * (team_set / 2);
    long new_node_id = id < n_procs / 2 ? node->left : node->right;
    if (!id) {
        /* Add new node to leader's list */
        *nodes = attach_node(*nodes, node);
    } else {
        free_node(node);
    }

    /* Split communicator */
    MPI_Comm new_team;
    int new_id, new_procs;
    MPI_Comm_split(team, id < (n_procs / 2), id, &new_team);
    MPI_Comm_rank(new_team, &new_id);
    MPI_Comm_size(new_team, &new_procs);

    /* Perform split
     * Approach: gather all L points at L team leader and then scatter them. Same for R
     * */
    long new_team_set = team_set / 2 + (team_set % 2 == 1 && id >= (n_procs / 2)); // Number of points my team will receive in total
    long new_set = new_team_set / new_procs + (new_id < (new_team_set % new_procs)); // Number of points I'll receive in total
    /* Alocate space for points */
    double *_p = (double*) malloc(n_dims * new_set * sizeof(double));
    double **new_pts = (double**) malloc(new_set * sizeof(double*));
    for (long i = 0; i < new_set; i++) {
        new_pts[i] = &_p[i * n_dims];
    }

    /* New team leaders allocate space for gather */
    double *team_points = NULL;
    if (!new_id) {
        team_points = (double*) malloc(new_team_set * n_dims * sizeof(double)); 
    }

    int rec_counts[n_procs], rec_displacements[n_procs];
    int L_count = split * n_dims;
    int R_count = (my_set - split) * n_dims;
    /* Gather counts */
    MPI_Gather(&L_count, 1, MPI_INT, rec_counts, 1, MPI_INT, 0, team);
    MPI_Gather(&R_count, 1, MPI_INT, rec_counts, 1, MPI_INT, n_procs / 2, team);
    int sum = 0;
    rec_displacements[0] = 0;
    if (!new_id) {
        for (long i = 1; i < n_procs; i++) {
            sum += rec_counts[i - 1];
            rec_displacements[i] = sum;
        }
    }
    /* Gather L points */
    MPI_Gatherv(pts[0], L_count, MPI_DOUBLE, team_points, rec_counts, rec_displacements, MPI_DOUBLE, 0, team);
    /* Gather R points */
    MPI_Gatherv(pts[split], R_count, MPI_DOUBLE, team_points, rec_counts, rec_displacements, MPI_DOUBLE, n_procs / 2, team);

    /* Scatter them */
    int send_counts[new_procs], send_displacements[new_procs];
    sum = 0;
    if (!new_id) {
        int remainder = new_team_set % new_procs;
        for (long i = 0; i < new_procs; i++) {
            send_counts[i] = (new_team_set / new_procs + (i < remainder)) * n_dims;
            send_displacements[i] = sum;
            sum += send_counts[i];
        }
    }
    MPI_Scatterv(team_points, send_counts, send_displacements, MPI_DOUBLE, _p, new_set * n_dims, MPI_DOUBLE, 0, new_team);

    if (team_points) free(team_points);
    free(*pts);
    free(pts);
    build_tree(new_pts, new_team, nodes, new_set, new_team_set, new_node_id);
}

#pragma region print

void print_node(node_t *node)
{
    printf("%ld %ld %ld %lf",
           node->id,
           node->left,
           node->right,
           node->radius);

    for (long i = 0; i < n_dims; i++)
    {
        printf(" %lf", node->center[i]);
    }
    printf(" \n");
}

void dump_tree(node_t *nodes)
{
    for (node_t *aux = nodes; aux != NULL; aux = aux->next)
        print_node(aux);
}

#pragma endregion print

int main(int argc, char *argv[])
{
    double exec_time = -omp_get_wtime();
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &id);
    MPI_Comm_size(MPI_COMM_WORLD, &n_procs);
    unsigned seed;
    double **pts = NULL;
    node_t *nodes = NULL;
    long my_set;
    MPI_Comm comm;

    if(argc != 4){
        printf("Usage: %s <n_dims> <n_points> <seed>\n", argv[0]);
        exit(1);
    }

    n_dims = atoi(argv[1]);
    if(n_dims < 2){
        printf("Illegal number of dimensions (%d), must be above 1.\n", n_dims);
        exit(2);
    }

    n_points = atol(argv[2]);
    if(n_points < 1){
        printf("Illegal number of points (%ld), must be above 0.\n", n_points);
        exit(3);
    }

    seed = atoi(argv[3]);
    srandom(seed);

    /* Create communicator without excess processors */
    if (n_points < n_procs) {
        MPI_Group old_group, new_group;
        int excess[n_procs - n_points];
        for (int i = 0; i < n_procs - n_points; i++) excess[i] = n_points + i;

        MPI_Comm_group(MPI_COMM_WORLD, &old_group);
        MPI_Group_excl(old_group, n_procs - n_points, excess, &new_group);
        MPI_Comm_create(MPI_COMM_WORLD, new_group, &comm);
    } else {
        comm = MPI_COMM_WORLD;
    }

    /* Spread points across rest of processors */
    if (!id) {
        printf("%d %ld\n", n_dims, 2 * n_points - 1);
        long pts_per_proc = n_points / n_procs;
        long remainder = n_points % n_procs;

        /* Get points for processor 0 */
        sprintf(argv[2], "%ld", pts_per_proc + (remainder > 0));
        remainder--;
        pts = get_points(argc, argv, &n_dims, &my_set);

        for (int i = 1; i < n_procs; i++) {
            long set_size;
            double **pts_for_i;
            sprintf(argv[2], "%ld", pts_per_proc + (remainder > 0));
            remainder--;

            if (atoi(argv[2]) > 0) {
                pts_for_i = get_points(argc, argv, &n_dims, &set_size);
                send_points(i, MPI_COMM_WORLD, pts_for_i, set_size);
                free(*pts_for_i);
                free(pts_for_i);
            } else {
                /* Send termination message */
                long message = TERMINATE;
                MPI_Send(&message, 1, MPI_LONG, i, PTS, MPI_COMM_WORLD);
            }
        }

        MPI_Comm_rank(comm, &id);
        MPI_Comm_size(comm, &n_procs);
        build_tree(pts, comm, &nodes, my_set, n_points, 0);
    } else {
        if (!receive_points(0, MPI_COMM_WORLD, &pts, &my_set)) {
            MPI_Comm_rank(comm, &id);
            MPI_Comm_size(comm, &n_procs);
            build_tree(pts, comm, &nodes, my_set, n_points, 0);
        }
    }
    MPI_Barrier(MPI_COMM_WORLD);

    exec_time += omp_get_wtime();

    MPI_Comm_rank(MPI_COMM_WORLD, &id);
    MPI_Comm_size(MPI_COMM_WORLD, &n_procs);

    if (!id) {
        fprintf(stderr, "%.1f\n", exec_time);
    }

    int send = PRINT, recv;
    /* print */
    if (n_procs < 2) {
        dump_tree(nodes);
    } else if (!id) {
        MPI_Send(&send, 1, MPI_INT, 1, 1, MPI_COMM_WORLD);
        MPI_Recv(&recv, 1, MPI_INT, n_procs - 1, 0, MPI_COMM_WORLD, &status);
        if (nodes) dump_tree(nodes);
    } else {
        MPI_Recv(&recv, 1, MPI_INT, id - 1, id, MPI_COMM_WORLD, &status);
        if (nodes) dump_tree(nodes);
        MPI_Send(&send, 1, MPI_INT, (id + 1) % n_procs, (id + 1) % n_procs, MPI_COMM_WORLD);
    }


    if (nodes) free_list(nodes);
    MPI_Finalize();
}
